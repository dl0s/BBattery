"""Deployment regression checks with simulated QNX output; no device changes."""
import pathlib
import sys
import unittest
import tempfile
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import device

DNAME = "top.blaccat.BBattery.testDev_at_BBattery8620bde8"


class DeviceToolsTest(unittest.TestCase):
    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "/apps/" + DNAME, "/sandbox"))
    @patch.object(device, "scp")
    @patch.object(device, "ssh", return_value="RESULT: PASS\nPASS: SQLite")
    def test_packaged_collector_is_verified_and_tested_without_root(self, ssh, scp, receipt):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            with zipfile.ZipFile(root / "BBattery.bar", "w") as archive:
                archive.writestr("native/batteryd", b"packaged-collector")
            scp.side_effect = lambda local, remote, download: pathlib.Path(local).write_bytes(b"packaged-collector")
            with patch.object(device, "BUILD", root):
                remote, digest, report = device.installed_collector("100", "902")
            self.assertEqual(remote, "/apps/" + DNAME + "/native/batteryd")
            self.assertEqual(ssh.call_args.args[0], "on -e TMPDIR=/sandbox/data/diagnostics/collector-self-test -u 100:902,1000 " + remote + " --self-test")
            self.assertEqual(len(digest), 64)
            self.assertIn("RESULT: PASS", report)

    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "/apps/" + DNAME, ""))
    @patch.object(device, "scp")
    @patch.object(device, "ssh")
    def test_changed_package_asset_is_never_executed(self, ssh, scp, receipt):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            with zipfile.ZipFile(root / "BBattery.bar", "w") as archive:
                archive.writestr("native/batteryd", b"expected")
            scp.side_effect = lambda local, remote, download: pathlib.Path(local).write_bytes(b"different")
            with patch.object(device, "BUILD", root):
                with self.assertRaisesRegex(RuntimeError, "differs from the package"):
                    device.installed_collector("100", "902")
            ssh.assert_not_called()

    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "", ""))
    @patch.object(device, "require_root")
    @patch.object(device, "scp")
    @patch.object(device, "ssh")
    def test_boot_hook_is_persistent_idempotent_and_preserves_other_services(self, ssh, scp, require_root, receipt):
        original = "#!/bin/sh\n/usr/sbin/sshd\necho existing-service\n"
        remote_files = {"boot": original.encode()}
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "collector-evidence.json").write_text("{}", encoding="utf-8")
            def transfer(local, remote, download=False):
                if download:
                    pathlib.Path(local).write_bytes(remote_files["boot"])
                else:
                    remote_files["next"] = pathlib.Path(local).read_bytes()
            def execute(command):
                if "boot-source.next" in command:
                    remote_files["boot"] = remote_files["next"]
                return ""
            scp.side_effect = transfer
            ssh.side_effect = execute
            with patch.object(device, "BUILD", root):
                device.install_boot_hook()
                once = remote_files["boot"]
                mutations = ssh.call_count
                device.install_boot_hook()
            self.assertTrue(once.startswith(original.encode()))
            self.assertEqual(remote_files["boot"], once)
            self.assertEqual(ssh.call_count, mutations)
            self.assertEqual(once.count(b"# BBATTERY COLLECTOR BOOT BEGIN"), 1)
            self.assertIn(device.SERVICE.encode() + b"/start.sh", once)
            self.assertNotIn(b"/var/bbattery", once)

    def test_deploy_profile_fallback_and_manager_precedence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            deploy = root / "Q10Deploy/config.json"
            deploy.parent.mkdir()
            deploy.write_text("{}", encoding="utf-8")
            self.assertEqual(device.connection_profile(root), deploy)
            manager = root / "Q10Manager/connection.json"
            manager.parent.mkdir()
            manager.write_text("{}", encoding="utf-8")
            self.assertEqual(device.connection_profile(root), manager)

    def test_missing_profile_fails_before_connecting(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                device.connection_profile(directory)

    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "", ""))
    @patch.object(device, "ssh")
    def test_pidin_header_does_not_hide_matching_gui(self, ssh, receipt):
        ssh.side_effect = [
            "     pid Arguments\n 303 " + DNAME + "\n 999 unrelated\n",
            "     pid Arguments\n 303 " + DNAME + "\n",
        ]
        self.assertEqual(device.gui_pids(), ["303"])

    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "", ""))
    @patch.object(device, "ssh")
    def test_reused_pid_is_not_a_gui_target(self, ssh, receipt):
        ssh.side_effect = [
            "     pid Arguments\n 303 " + DNAME + "\n",
            "     pid Arguments\n 303 unrelated\n",
        ]
        self.assertEqual(device.gui_pids(), [])

    @patch.object(device, "gui_pids", return_value=["303"])
    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "", ""))
    @patch.object(device, "ssh")
    def test_existing_gui_is_not_launched_twice(self, ssh, receipt, pids):
        device.launch()
        ssh.assert_not_called()

    @patch.object(device, "gui_pids", side_effect=[[], ["303"]])
    @patch.object(device, "receipt", return_value=({"dname": DNAME}, "", ""))
    @patch.object(device, "ssh")
    def test_launch_uses_unique_request_and_verifies_process(self, ssh, receipt, pids):
        device.launch()
        self.assertEqual(ssh.call_count, 1)
        self.assertRegex(ssh.call_args.args[0], r"id::bbattery-[0-9a-f]{32}")
        self.assertEqual(pids.call_count, 2)

    @patch.object(device, "gui_pids", side_effect=[["303", "304"], []])
    @patch.object(device, "ssh")
    def test_stop_verifies_exit_and_targets_only_gui(self, ssh, pids):
        device.stop_gui()
        self.assertEqual([call.args[0] for call in ssh.call_args_list], ["kill 303", "kill 304"])

    @patch.object(device, "collector_files", return_value=[])
    @patch.object(device, "upload_verified")
    @patch.object(device, "scp")
    @patch.object(device, "ssh")
    def test_collector_is_verified_before_first_execution(self, ssh, scp, upload, files):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            binary = root / "binary"
            binary.write_bytes(b"isolated-test-binary")
            def transfer(local, remote, download=False):
                if download:
                    pathlib.Path(local).write_bytes(binary.read_bytes())
            scp.side_effect = transfer
            ssh.side_effect = ["", ": trusted", "RESULT: PASS\nPASS: SQLite", ""]
            with patch.object(device, "BUILD", root):
                device.stage_collector(binary)
        commands = [call.args[0] for call in ssh.call_args_list]
        self.assertTrue(upload.call_args_list[0].args[1].endswith(".source"))
        self.assertIn("test ! -e", commands[0])
        self.assertIn("cp ", commands[0])
        self.assertIn("pathtrust ", commands[1])
        self.assertIn("--self-test", commands[2])
        self.assertFalse(any("mv " in command for command in commands))

    @patch.object(device, "collector_files", return_value=[])
    @patch.object(device, "upload_verified")
    @patch.object(device, "scp")
    @patch.object(device, "ssh")
    def test_trust_failure_never_executes_collector(self, ssh, scp, upload, files):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            binary = root / "binary"
            binary.write_bytes(b"isolated-test-binary")
            def transfer(local, remote, download=False):
                pathlib.Path(local).write_bytes(binary.read_bytes())
            scp.side_effect = transfer
            ssh.side_effect = ["", "not registered"]
            with patch.object(device, "BUILD", root):
                with self.assertRaisesRegex(RuntimeError, "trust was not reported"):
                    device.stage_collector(binary)
        self.assertFalse(any("--self-test" in call.args[0] for call in ssh.call_args_list))

    @patch.object(device, "scp")
    @patch.object(device, "ssh")
    def test_retired_fallback_never_writes_active_or_supervised_files(self, ssh, scp):
        running = "/var/bbattery/bin/batteryd-" + "a" * 64
        supervised = "/var/bbattery/bin/batteryd-" + "b" * 64
        ssh.side_effect = [" 303 " + running + " --data /private\n", "#!/bin/ksh\n" + supervised + " --data /private\n"]
        with self.assertRaisesRegex(RuntimeError, "running service unchanged"):
            device.reuse_retired_collector([running, supervised], "/source", "/new", "c" * 64)
        scp.assert_not_called()

    @patch.object(device, "ssh")
    def test_collector_candidates_exclude_symlinks_and_shared_writable_files(self, ssh):
        name = "batteryd-" + "a" * 64
        ssh.return_value = ("-rwx------ 1 0 0 123 Oct 01 12:00 " + name + "\n" +
                            "-rwxrwxrwx 1 0 0 123 Oct 01 12:00 " + name + "-12345678\n" +
                            "lrwx------ 1 0 0 123 Oct 01 12:00 " + name + "-87654321\n")
        self.assertEqual(device.collector_files(), ["/var/bbattery/bin/" + name])


if __name__ == "__main__":
    unittest.main(verbosity=2)
