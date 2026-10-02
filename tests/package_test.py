"""Package acceptance for the focused interval-test product."""
import pathlib
import unittest
import xml.etree.ElementTree as ET
import zipfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
class PackageTest(unittest.TestCase):
    def test_descriptor(self):
        root=ET.parse(ROOT/'bar-descriptor.xml').getroot()
        ns={'q':'http://www.qnx.com/schemas/application/1.0'}
        self.assertEqual(root.find('q:id',ns).text,'top.blaccat.BBattery')
        self.assertEqual(root.find('q:buildId',ns).text,'7')
        self.assertNotIn('run_when_backgrounded',[p.text for p in root.findall('q:permission',ns)])
    def test_bar_matches_current_sources(self):
        with zipfile.ZipFile(ROOT/'build/BBattery.bar') as bar:
            self.assertIn('Package-Name: top.blaccat.BBattery',bar.read('META-INF/MANIFEST.MF').decode())
            for name,path in [('native/bbattery','build/bbattery'),('native/assets/main.qml','assets/main.qml'),('native/assets/Metric.qml','assets/Metric.qml')]:
                self.assertEqual(bar.read(name),(ROOT/path).read_bytes())
            self.assertNotIn('native/assets/Chart.qml',bar.namelist())
            for name in bar.namelist():
                self.assertFalse(name.endswith(('.pem','.key','.p12')))
    def test_focused_test_workflow(self):
        qml=(ROOT/'assets/main.qml').read_text(encoding='utf-8-sig')
        self.assertEqual(qml.count('    Tab {'),2)
        for name in ('backend.startTest','backend.stopTest','backend.selectTest','backend.useBattery','backend.renameBattery','backend.loadMore','backend.exportData'):
            self.assertIn(name,qml)
        for name in ('Chart {','DateTimePicker','BatteryGauge {','configureAlerts','History'):
            self.assertNotIn(name,qml)
    def test_only_tests_sample(self):
        collector=(ROOT/'src/collector.cpp').read_text(encoding='utf-8-sig')
        self.assertNotIn('battery_request_events(',collector)
        self.assertNotIn('alerts.evaluate',collector)
        self.assertIn('!db.runningTest().isEmpty()&&(now>=next||now>=deadline)',collector)
        self.assertIn('idle?30000:10000',collector)
        self.assertIn('test/expires',collector)
    def test_reading_and_export_run_off_ui(self):
        backend=(ROOT/'src/backend.cpp').read_text(encoding='utf-8-sig')
        self.assertIn('class TestReader:public QThread',backend)
        self.assertIn('class ExportWorker:public QThread',backend)
        for name in ('QPainter','QFont','HistoryLoader','OverviewLoader','db.history('):
            self.assertNotIn(name,backend)
        self.assertIn('pending()?1000:running()?5000:10000',backend)
    def test_native_measurement_coverage(self):
        src=(ROOT/'src/collector.cpp').read_text(encoding='utf-8-sig')
        for name in ('deadline clips charge','test update and sample transaction roll back','restart persists and interrupts','mixed direction refuses capacity','missing SOC and voltage','interior SOC reversal'):
            self.assertIn(name,src)
if __name__=='__main__':
    unittest.main(verbosity=2)
