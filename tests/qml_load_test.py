"""Native SDK page loading and regressions for the real 0.1.0.10 QML failure."""
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import qml_preview


class QmlLoadTest(unittest.TestCase):
    target = pathlib.Path('C:/bbdevtools/target_10_3_1_995')

    def check_render(self, page, overrides=None):
        if not (self.target / 'win32/x86/usr/bin/qmlpreview.exe').is_file():
            self.skipTest('BB10 Windows Cascades preview unavailable')
        return qml_preview.render(self.target, ROOT / 'build/qml-regression', page, 'normal', overrides=overrides)

    def test_all_production_pages_create_and_render(self):
        for page in qml_preview.PAGES:
            with self.subTest(page=page):
                report = self.check_render(page)
                self.assertEqual(report['errors'], [], report)
                self.assertTrue(report['files'])

    def test_missing_action_comma_is_rejected_by_native_parser(self):
        source = (ROOT / 'assets/main.qml').read_text(encoding='utf-8')
        broken = source.replace('backend.exportData(true) },', 'backend.exportData(true) }', 1)
        self.assertNotEqual(source, broken)
        report = self.check_render('main.qml', {'main.qml': broken})
        self.assertTrue(any('Expected token' in error for error in report['errors']), report)
        self.assertEqual(report['files'], [])

    def test_object_property_semicolon_is_rejected_by_native_parser(self):
        source = (ROOT / 'assets/main.qml').read_text(encoding='utf-8')
        broken = source.replace('StackLayoutProperties { spaceQuota: 1 } textStyle', 'StackLayoutProperties { spaceQuota: 1 }; textStyle', 1)
        self.assertNotEqual(source, broken)
        report = self.check_render('main.qml', {'main.qml': broken})
        self.assertTrue(any('Unexpected token' in error for error in report['errors']), report)
        self.assertEqual(report['files'], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
