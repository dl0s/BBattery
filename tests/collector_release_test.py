"""Validate application startup boundary, without claiming device lifecycle acceptance."""
import ast
import pathlib
import sys
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class CollectorReleaseTest(unittest.TestCase):
    def test_current_tools_have_no_transport_or_privileged_hook(self):
        for path in (ROOT/'tools').glob('*.py'):
            source = path.read_text(encoding='utf-8-sig')
            ast.parse(source)
            for retired in ('with_device_lock.ps1','WinDLL','CreateFileW','192.168.1.61','Q10Deploy/config.json','/tmp/'):
                self.assertNotIn(retired,source,str(path))
        self.assertNotIn('btool',(ROOT/'tools/device.py').read_text())
    def test_startup_is_os_headless_and_health_guarded(self):
        source = (ROOT/'src/backend.cpp').read_text(encoding='utf-8-sig')
        startup = source.split('bool Backend::connectCollector()',1)[1].split('QVariantMap Backend::formatTest',1)[0]
        self.assertIn('CollectorStopped',startup)
        self.assertIn('collectorInvoker->invoke(invocation)',startup)
        self.assertNotIn('QProcess::startDetached',startup)
        self.assertNotIn('setuid',startup)
        self.assertNotIn('pathtrust',startup.lower())
        self.assertIn('collectorLaunchAttempted',source)
    def test_ready_keeps_lock_instance_and_business_heartbeat_checks(self):
        source = (ROOT/'src/collector_state.h').read_text(encoding='utf-8-sig')
        for required in ('flock','collector.instance','run_id','euid','mono_ms','45000'):
            self.assertIn(required,source)
    def test_device_script_boundary_is_explicit(self):
        # This release contains ARM native programs and QML, no new Python/ksh payload.
        import xml.etree.ElementTree as ET
        descriptor=ET.parse(ROOT/'bar-descriptor.xml').getroot()
        assets=descriptor.findall('{http://www.qnx.com/schemas/application/1.0}asset')
        self.assertFalse(any(asset.attrib['path'].endswith(('.py','.ksh','.sh')) for asset in assets))
if __name__ == '__main__': unittest.main(verbosity=2)
