"""Local package checks; never claim a device pass."""
import pathlib
import unittest
import xml.etree.ElementTree as ET
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PackageTest(unittest.TestCase):
    def test_descriptor(self):
        q = ET.parse(ROOT / "bar-descriptor.xml").getroot()
        ns = {"q": "http://www.qnx.com/schemas/application/1.0"}
        self.assertEqual(q.find("q:id", ns).text, "top.blaccat.BBattery")
        self.assertEqual(q.find("q:buildId", ns).text, "5")

    def test_bar_assets_and_identity(self):
        with zipfile.ZipFile(ROOT / "build/BBattery.bar") as bar:
            self.assertIn("Package-Name: top.blaccat.BBattery", bar.read("META-INF/MANIFEST.MF").decode())
            for name in ("bbattery",):
                self.assertEqual(bar.read("native/" + name), (ROOT / "build" / name).read_bytes())
            for name in ("main.qml", "Metric.qml", "Chart.qml", "BatteryGauge.qml"):
                self.assertIn("native/assets/" + name, bar.namelist())
                self.assertEqual(bar.read("native/assets/" + name), (ROOT / "assets" / name).read_bytes())
            for name in bar.namelist():
                self.assertNotIn("private", name.lower())
                self.assertFalse(name.endswith((".pem", ".key", ".p12")))
            qml = bar.read("native/assets/main.qml").decode("utf-8")
            self.assertEqual(qml.count("    Tab {"), 4)
            self.assertIn("系统健康度", qml)
            self.assertIn("电池平均电流", qml)
            self.assertNotIn("titleBar:", qml)
            for source in ("放电积分", "充电积分", "余量 / SOC", "健康度折算", "系统满充", "设计容量"):
                self.assertIn(source, qml)
            self.assertIn("时间范围", qml)
            self.assertIn("DateTimePicker", qml)

    def test_missing_values_and_native_tests_exist(self):
        source = (ROOT / "src/measurement.cpp").read_text(encoding="utf-8")
        self.assertIn("value==80000000.0", source)
        self.assertIn("restart does not integrate", source)
        self.assertIn("missing current is not zero", source)
        self.assertIn("short SOC range is insufficient", source)

    def test_charts_do_not_use_qt_font_rendering(self):
        source = (ROOT / "src/backend.cpp").read_text(encoding="utf-8")
        self.assertNotIn("QFont", source)
        self.assertNotIn("drawText(", source)
        qml = (ROOT / "assets/Chart.qml").read_text(encoding="utf-8")
        self.assertIn("axes", qml)
        self.assertIn("Label {", qml)

    def test_progressive_disclosure(self):
        qml = (ROOT / "assets/main.qml").read_text(encoding="utf-8")
        overview = qml.split("id: liveTab", 1)[1].split("id: trendTab", 1)[0]
        for name in ("designText", "healthText", "inputLimitText", "estimateText"):
            self.assertNotIn(name, overview)
        self.assertIn("sessionCount", qml)
        self.assertNotIn("sessions.size()", qml)

    def test_complete_history_and_alerts(self):
        source = (ROOT / "src/backend.cpp").read_text(encoding="utf-8")
        self.assertNotIn("LIMIT 24000", source)
        self.assertIn("value!=168&&value!=720", source)
        collector = (ROOT / "src/collector.cpp").read_text(encoding="utf-8")
        self.assertIn("battery_get_domain()", collector)
        self.assertIn("lastSample>=2000", collector)
        self.assertIn("recordEvent(s,kind,text,600)", collector)
        self.assertIn("class HistoryLoader:public QThread", source)
        store = (ROOT / "src/store.cpp").read_text(encoding="utf-8")
        self.assertIn("LIMIT 512", store)
        self.assertIn("maximumId", store)

    def test_inspection_does_not_redraw_or_query_on_touch(self):
        source = (ROOT / "src/backend.cpp").read_text(encoding="utf-8")
        inspection = source.split("void Backend::inspect(", 1)[1].split("bool Backend::saveSettings", 1)[0]
        self.assertNotIn("=chart(", inspection)
        self.assertNotIn("emit chartsChanged()", inspection)
        self.assertNotIn("db.query(", inspection)
        self.assertIn("inspectionTimer.start(33)", inspection)
        self.assertIn("inspectionLoader->generation==inspectionGeneration", inspection)
        self.assertIn("class InspectionLoader:public QThread", source)
        chart = (ROOT / "assets/Chart.qml").read_text(encoding="utf-8")
        self.assertIn("cursor.visible", chart)
        self.assertNotIn("axes.cursor", chart)
        qml = (ROOT / "assets/main.qml").read_text(encoding="utf-8")
        self.assertEqual(qml.count("cursor: backend.inspectionCursor"), 2)
        readout = qml.split("text: backend.inspection", 1)[1].split("Chart {", 1)[0]
        self.assertIn("minHeight: preferredHeight; maxHeight: preferredHeight", readout)
        self.assertNotIn("visible:", readout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
