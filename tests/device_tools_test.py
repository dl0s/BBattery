"""Host checks for the coordinated BBattery observers and release entry."""
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import device
import collector_release
import device_lease

DNAME="top.blaccat.BBattery.testDev_at_BBattery8620bde8"

class DeviceToolsTest(unittest.TestCase):
    def test_install_provision_and_boot_use_the_same_release_entry(self):
        with patch.object(collector_release,"install",return_value="verified") as deploy:
            self.assertEqual(device.install(),"verified")
            self.assertEqual(device.provision(),"verified")
            self.assertEqual(device.install_boot_hook(),"verified")
            self.assertEqual(deploy.call_count,3)

    def test_profile_prefers_manager_and_preserves_deploy_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory)
            with self.assertRaises(FileNotFoundError):device.connection_profile(root)
            deploy=root/"Q10Deploy/config.json";deploy.parent.mkdir();deploy.write_text("{}")
            self.assertEqual(device.connection_profile(root),deploy)
            manager=root/"Q10Manager/connection.json";manager.parent.mkdir();manager.write_text("{}")
            self.assertEqual(device.connection_profile(root),manager)

    @patch.object(device,"receipt",return_value=({"dname":DNAME},"", ""))
    @patch.object(device,"ssh")
    def test_gui_pid_requires_exact_current_arguments(self,ssh,receipt):
        ssh.side_effect=[" 303 "+DNAME+"\n 304 /apps/"+DNAME+"/native/batteryd --data /private\n"," 303 "+DNAME+"\n"]
        self.assertEqual(device.gui_pids(),["303"])
        ssh.side_effect=[" 303 "+DNAME+"\n"," 303 unrelated\n"]
        self.assertEqual(device.gui_pids(),[])

    @patch.object(device,"receipt",return_value=({"dname":DNAME},"", ""))
    @patch.object(device,"gui_pids",return_value=["303"])
    @patch.object(device,"ssh")
    def test_launch_skips_live_gui(self,ssh,pids,receipt):
        with tempfile.TemporaryDirectory() as directory,patch.dict('os.environ',LOCALAPPDATA=directory):
            device.launch();ssh.assert_not_called()

    def test_lease_is_reentrant_and_keeps_lock_file(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict("os.environ",LOCALAPPDATA=directory):
            with device_lease.lease():
                with device_lease.lease():pass
                self.assertTrue(device_lease._local.held)
            self.assertFalse(device_lease._local.held)
            self.assertEqual(len(list(pathlib.Path(directory).rglob("*.lock"))),1)

    def test_lease_refuses_unconfirmed_manager_job(self):
        with tempfile.TemporaryDirectory() as directory,patch.dict("os.environ",LOCALAPPDATA=directory):
            job=pathlib.Path(directory)/"Q10Manager/Operations/v1/jobs/pending";job.mkdir(parents=True)
            (job/"state.json").write_text('{"status":"unconfirmed","id":"original"}')
            (job/"request.json").write_text('{"connection":{"deviceHost":"192.168.1.61"}}')
            with self.assertRaisesRegex(RuntimeError,"requires attention"):
                with device_lease.lease():self.fail("Acquired unresolved queue")

if __name__=="__main__":unittest.main(verbosity=2)
