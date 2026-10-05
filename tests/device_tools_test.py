"""Host protocol-client checks; no device commands or real queue mutations."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import device

class DeviceToolsTest(unittest.TestCase):
    def test_no_legacy_profile_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            old = root/'Q10Deploy/config.json'; old.parent.mkdir(); old.write_text('{}')
            with self.assertRaises(FileNotFoundError): device.connection_profile(root)
            current = root/'Q10Manager/connection.json'; current.parent.mkdir(); current.write_text('{}')
            self.assertEqual(device.connection_profile(root),current)

    def test_host_resolves_its_own_verified_pin_even_with_same_host_key(self):
        devices = [{'pin':'AAAAAAAA','endpoints':[{'deviceHost':'192.168.1.61','expectedPin':'AAAAAAAA','sshKeyPath':'one','hostPublicKey':'same'}]},
                   {'pin':'BBBBBBBB','endpoints':[{'deviceHost':'192.168.1.64','expectedPin':'BBBBBBBB','sshKeyPath':'two','hostPublicKey':'same'}]}]
        with patch.object(device,'cli',return_value=json.dumps(devices)):
            self.assertEqual(device.connection('192.168.1.61')['sshKeyPath'],'one')
            self.assertEqual(device.connection('192.168.1.64')['sshKeyPath'],'two')
            with self.assertRaises(ValueError): device.connection('unknown')

    def test_stable_intent_precedes_submit_and_restart_only_queries(self):
        with tempfile.TemporaryDirectory() as directory:
            intent = pathlib.Path(directory)/'intent.json'
            request = {'kind':'build','projectDirectory':'fixture'}
            def call(command,*arguments,**kwargs):
                saved = json.loads(intent.read_text())
                if command == 'submit':
                    self.assertEqual(saved['phase'],'submission-may-have-started')
                    self.assertEqual(json.loads(pathlib.Path(arguments[0]).read_text())['id'],saved['id'])
                    return saved['id']
                self.assertEqual(command,'status');return '{}'
            with patch.object(device,'cli',side_effect=call) as cli:
                first = device.submit(request,intent)
                self.assertEqual(device.submit(request,intent),first)
                self.assertEqual([args.args[0] for args in cli.call_args_list],['submit','status'])
                with self.assertRaises(ValueError): device.submit(dict(request,projectDirectory='changed'),intent)

    def test_lost_submission_response_never_replays(self):
        with tempfile.TemporaryDirectory() as directory:
            intent = pathlib.Path(directory)/'intent.json'
            request = {'kind':'build'}
            with patch.object(device,'cli',side_effect=subprocess.TimeoutExpired('submit',45)):
                with self.assertRaisesRegex(RuntimeError,'original ID'): device.submit(request,intent)
            original = json.loads(intent.read_text())['id']
            with patch.object(device,'cli',return_value='{}') as cli:
                self.assertEqual(device.submit(request,intent),original)
                cli.assert_called_once_with('status',original,store=None)

    def test_unconfirmed_result_is_not_success(self):
        values = {'status':json.dumps({'state':{'status':'unconfirmed'}}),'events':'[]','failure':'{"code":"TERMINAL_MISSING"}'}
        with patch.object(device,'cli',side_effect=lambda command,*args,**kwargs:values[command]) as cli:
            result = device.observe('a'*32)
            self.assertNotIn('result',result)
            self.assertEqual(result['failure']['code'],'TERMINAL_MISSING')

    def test_original_transaction_observation_uses_original_snapshot_and_hash(self):
        original = {'request':{'kind':'deploy','connection':{'expectedPin':'AAAAAAAA'},'packageSha256':'b'*64}}
        with patch.object(device,'cli',return_value=json.dumps(original)),patch.object(device,'submit',return_value='c'*32) as submit:
            self.assertEqual(device.continue_deployment('a'*32),'c'*32)
            request = submit.call_args.args[0]
            self.assertEqual(request['originalDeploymentId'],'a'*32)
            self.assertEqual(request['packageSha256'],'b'*64)
            self.assertNotIn('packagePath',request)

    def test_legacy_device_operations_and_workers_fail_closed(self):
        for method in (device.ssh,device.scp,device.launch,device.provision,device.install_boot_hook,device.app_call):
            with self.assertRaisesRegex(RuntimeError,'no BBattery'): method('anything')
        for command in ('worker','run-once','acknowledge','abandon','delete','cancel','ssh'):
            with self.assertRaises(ValueError): device.cli(command)

    def test_persistent_record_failure_prevents_submission(self):
        with tempfile.TemporaryDirectory() as directory,patch.object(device,'save',side_effect=OSError('full')),patch.object(device,'cli') as cli:
            with self.assertRaises(OSError): device.submit({'kind':'build'},pathlib.Path(directory)/'intent.json')
            cli.assert_not_called()

    def test_actual_cli_isolated_build_submission_and_original_read(self):
        if not device.CLI.is_file(): self.skipTest('Published CLI unavailable')
        with tempfile.TemporaryDirectory() as directory:
            request = {'kind':'build','projectDirectory':str(ROOT),'sdkDirectory':'C:/bbdevtools','buildScriptPath':str(ROOT/'build.ps1'),'buildOutputPath':str(ROOT/'build/BBattery.bar'),'buildSwitches':['Package']}
            intent = pathlib.Path(directory)/'client/intent.json'; store = pathlib.Path(directory)/'isolated-store'
            first = device.submit(request,intent,store)
            self.assertEqual(device.observe(first,store)['state']['status'],'queued')
            self.assertEqual(device.submit(request,intent,store),first)
            self.assertEqual(len(list((store/'jobs').iterdir())),1)

if __name__ == '__main__': unittest.main(verbosity=2)
