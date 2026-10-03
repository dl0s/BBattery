"""Fault tests for one-shot maintenance; no Q10 mutations."""
import base64
import hashlib
import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch, mock_open

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import collector_maintenance as m
import collector_release as host


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.service = types.SimpleNamespace(JOBS='/pps/jobs', RUN='/run',
            unfinished=lambda: [], status_ready=lambda state: True,
            atomic_json=lambda path, state: None)
        m.service = self.service
        boot=b'#!/bin/sh\nroot-and-ssh\n# BBATTERY COLLECTOR BOOT BEGIN\nold\n# BBATTERY COLLECTOR BOOT END\nunsigned-and-tail\n'
        baseline=dict(bootSha256=m.digest(boot),bootBase64=base64.b64encode(boot).decode(),
                      startSha256=m.digest(b'old'),bootMetadata=[0o700,0,0],uid=100,gid=902)
        m.PLAN=dict(action='install',tag='bbattery-fault-test',old=dict(sha256='old',version='0.1.0.8',assets={},base64=''),
                    new=dict(sha256='new',version='0.1.0.9',assets={},base64=''),baseline=baseline,
                    starter=host.starter(100,902,m.DNAME))
        self.sign()
        m.record={"backups":{}}

    def sign(self):
        intent=dict(m.PLAN);intent.pop('action',None);intent.pop('intentSha256',None)
        m.PLAN['intentSha256']=m.digest(json.dumps(intent,sort_keys=True,ensure_ascii=True).encode())

    def test_frozen_mismatch_blocks_maintenance(self):
        with patch.object(m,'read',return_value=b'changed'):
            with self.assertRaisesRegex(RuntimeError,'Frozen unsigned'):m.barrier()

    def test_other_unfinished_job_blocks_maintenance(self):
        self.service.unfinished=lambda:['job.other']
        with patch.object(m,'EXPECTED',{'source':m.digest(b'original')}),patch.object(m,'read',return_value=b'original'),patch('builtins.open',mock_open(read_data='{}')):
            with self.assertRaisesRegex(RuntimeError,'unfinished'):m.barrier('job.owned')

    def test_progress_100_is_pending_and_never_resubmitted(self):
        record={'submission':{'job':'job.original','status':'unconfirmed'}}
        with patch.object(m,'barrier'),patch.object(m,'phase'),patch.object(m,'pps',return_value={'progress':'100'}),patch.object(m.os.path,'exists',return_value=True),patch.object(m.os,'open') as submit,patch.object(m.time,'time',side_effect=[0,1,30]),patch.object(m.time,'sleep'):
            self.assertFalse(m.native_install(record,m.PLAN['new']))
            submit.assert_not_called()
            self.assertEqual(record['submission']['status'],'unconfirmed')

    def test_missing_original_job_stays_unconfirmed(self):
        with patch.object(m,'barrier'),patch.object(m.os.path,'exists',return_value=False),patch.object(m.os,'open') as submit:
            with self.assertRaisesRegex(RuntimeError,'no replacement'):m.native_install({'submission':{}},m.PLAN['new'])
            submit.assert_not_called()

    def test_unrecognized_result_is_not_a_terminal(self):
        with patch.object(m,'barrier'),patch.object(m,'phase'),patch.object(m,'pps',return_value={'result':'processing','progress':'100'}),patch.object(m.os.path,'exists',return_value=True),patch.object(m.time,'time',side_effect=[0,1,30]),patch.object(m.time,'sleep'):
            self.assertFalse(m.native_install({'submission':{}},m.PLAN['new']))

    def test_success_requires_installed_assets(self):
        reply=dict(result='success',actual_dname=m.DNAME,actual_app_version='0.1.0.9')
        with patch.object(m,'barrier'),patch.object(m,'phase'),patch.object(m,'pps',return_value=reply),patch.object(m.os.path,'exists',return_value=True),patch.object(m,'version',return_value='0.1.0.9'),patch.object(m,'assets',return_value={'native/batteryd':'changed'}):
            with self.assertRaisesRegex(RuntimeError,'Installed assets'):m.native_install({'submission':{}},m.PLAN['new'])

    def test_parameter_change_blocks_before_stopping(self):
        m.PLAN['starter']+='changed'
        with patch.object(m,'stop_collector') as stop:
            with self.assertRaisesRegex(RuntimeError,'parameters differ'):m.update()
            stop.assert_not_called()

    def test_backup_failure_precedes_any_stop_or_submit(self):
        def read(path,*args):return b'' if path.endswith('.bar') else (b'old' if path.endswith('start.sh') else base64.b64decode(m.PLAN['baseline']['bootBase64']))
        with patch.object(m,'verify_image'),patch.object(m,'source_guard'),patch.object(m,'idle'),patch.object(m,'version',return_value='0.1.0.8'),patch.object(m,'assets',return_value={}),patch.object(m,'barrier',return_value=({},[])),patch.object(m.os.path,'exists',return_value=False),patch.object(m.os,'makedirs'),patch.object(m,'read',side_effect=read),patch.object(m,'write'),patch.object(m,'backup_data',side_effect=IOError('full')),patch.object(m,'stop_collector') as stop,patch.object(m,'stop_gui') as gui,patch.object(m,'native_install') as install:
            with self.assertRaisesRegex(IOError,'full'):m.update()
            stop.assert_not_called();gui.assert_not_called();install.assert_not_called()

    def test_unknown_startup_source_is_never_overwritten(self):
        with patch.object(m,'read',return_value=b'unrelated'):
            with self.assertRaisesRegex(RuntimeError,'outside this update'):m.source_guard()

    def test_unknown_effective_group_prevents_gui_signal(self):
        with patch.object(m,'gui_pids',return_value=[11]),patch.object(m,'process_identity',return_value=(100,903)),patch.object(m.os,'kill') as signal:
            with self.assertRaisesRegex(RuntimeError,'Unexpected GUI identity'):m.stop_gui()
            signal.assert_not_called()

    def test_new_boot_retains_every_other_byte(self):
        original=base64.b64decode(m.PLAN['baseline']['bootBase64'])
        new=m.new_boot()
        self.assertTrue(new.startswith(original.split(b'# BBATTERY COLLECTOR BOOT BEGIN')[0]))
        self.assertTrue(new.endswith(b'unsigned-and-tail\n'))
        self.assertIn(b'if /proc/boot/pathtrust',new)

    def test_consistent_database_and_settings_are_backed_up_before_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            data=pathlib.Path(directory)/'data';data.mkdir()
            backup=pathlib.Path(directory)/'backup';backup.mkdir()
            with sqlite3.connect(data/'history.sqlite') as connection:
                connection.execute('CREATE TABLE preserved(value TEXT)')
                connection.execute("INSERT INTO preserved VALUES('B1')")
            connection.close()
            (data/'settings.ini').write_bytes(b'[battery]\nactive=B1\n')
            with patch.object(m,'DATA',str(data)),patch.object(m,'idle'),patch.object(m.os,'chown',create=True):
                m.backup_data(str(backup))
            with sqlite3.connect(backup/'history.sqlite') as connection:
                self.assertEqual(connection.execute('SELECT value FROM preserved').fetchone()[0],'B1')
                self.assertEqual(connection.execute('PRAGMA integrity_check').fetchone()[0],'ok')
            connection.close()
            self.assertEqual(set(m.record['backups']),{'history.sqlite','settings.ini'})

    def test_shared_installed_asset_is_readable_but_backup_is_strict(self):
        with tempfile.TemporaryDirectory() as directory:
            path=pathlib.Path(directory)/'icon.png';path.write_bytes(b'icon')
            os.link(path,path.with_name('second.png'))
            self.assertEqual(m.read(str(path),single_link=False),b'icon')
            with self.assertRaisesRegex(RuntimeError,'Unexpected fixed file'):m.read(str(path))

    def test_incomplete_recovery_backup_blocks_stopping(self):
        m.PLAN['action']='recover'
        saved=dict(intentSha256=m.PLAN['intentSha256'],phase='native_result',backups={})
        with patch.object(m,'verify_image'),patch.object(m,'source_guard'),patch.object(m.os.path,'exists',side_effect=[True,False]),patch('builtins.open',mock_open(read_data=json.dumps(saved))),patch.object(m,'stop_collector') as stop,patch.object(m,'native_install') as install:
            with self.assertRaisesRegex(RuntimeError,'Incomplete recovery'):m.update()
            stop.assert_not_called();install.assert_not_called()

    def test_invalid_app_identity_is_refused(self):
        for uid,gid,name in [(0,902,m.DNAME),(100,0,m.DNAME),(100,902,'other'),(100,902,m.DNAME+';cmd')]:
            with self.assertRaises(ValueError):host.starter(uid,gid,name)

    @unittest.skipUnless(pathlib.Path('C:/cygwin64/bin/bash.exe').exists(), 'Cygwin shell unavailable')
    def test_bounded_start_refuses_unknown_owner_and_stops_after_three_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory)
            (root/'on').write_text('#!/bin/bash\ncase "$*" in *--status*) exit "$HEALTH";; *) echo launch >> "$COUNT"; exit 0;; esac\n',newline='\n')
            (root/'sleep').write_text('#!/bin/bash\nexit 0\n',newline='\n')
            (root/'start.sh').write_text(host.starter(100,902,m.DNAME),newline='\n')
            posix='/cygdrive/'+root.drive[0].lower()+root.as_posix()[2:]
            for health,expected,launches in [(0,0,0),(78,78,0),(11,78,0),(10,78,3)]:
                count=root/'count'
                if count.exists():count.unlink()
                command='PATH="'+posix+':/usr/bin:/bin" HEALTH='+str(health)+' COUNT="'+posix+'/count" bash "'+posix+'/start.sh"'
                reply=subprocess.run(['C:/cygwin64/bin/bash.exe','-c',command],capture_output=True,timeout=10)
                self.assertEqual(reply.returncode,expected,reply.stderr)
                self.assertEqual(len(count.read_text().splitlines()) if count.exists() else 0,launches)


if __name__=='__main__':unittest.main(verbosity=2)
