"""Deployment regression checks with simulated QNX output; no device changes."""
import pathlib
import sys
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import device

DNAME = "top.blaccat.BBattery.testDev_at_BBattery8620bde8"


class DeviceToolsTest(unittest.TestCase):
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

    @patch.object(device, "upload_verified")
    @patch.object(device, "scp")
    @patch.object(device, "ssh")
    def test_collector_is_verified_before_first_execution(self, ssh, scp, upload):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            binary = root / "binary"
            binary.write_bytes(b"isolated-test-binary")
            def transfer(local, remote, download=False):
                if download:
                    pathlib.Path(local).write_bytes(binary.read_bytes())
            scp.side_effect = transfer
            ssh.side_effect = ["RESULT: PASS\nPASS: SQLite", ": trusted", ""]
            with patch.object(device, "BUILD", root):
                device.stage_collector(binary)
        commands = [call.args[0] for call in ssh.call_args_list]
        self.assertTrue(upload.call_args.args[1].endswith(".source"))
        self.assertTrue(any("test ! -e" in command and "cp " in command and "pathtrust " in command and "--self-test" in command for command in commands))
        self.assertFalse(any("mv " in command for command in commands))


if __name__ == "__main__":
    unittest.main(verbosity=2)
