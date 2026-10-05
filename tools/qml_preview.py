"""Load production QML in the BB10 SDK's native Cascades preview.

All fixtures and bb.system stubs are generated under build/, never packaged.
The SDK Windows renderer does not provide the device-only bb.system plugin.
This verifies Cascades parsing/type creation; system dialogs need device validation.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
PAGES = ('main.qml', 'BatteryDetail.qml', 'TestPage.qml', 'TestRecords.qml', 'TestResult.qml')

BACKEND = '''import bb.cascades 1.4
import QtQuick 1.0
QtObject {
    signal notified(string message)
    property bool ready: true
    property bool running: false
    property bool pending: false
    property bool hasMore: false
    property int resultCount: 0
    property string status: "采集器就绪 · 待机不采样"
    property variant battery: ({viewedKey:"preview-a", activeKey:"preview-a", viewedLabel:"主用电池", activeLabel:"主用电池"})
    property variant capacity: ({available:true, valueText:"1,850", sourceType:"discharge-test", sourceText:"放电估计", timeText:"2026-10-04 18:30", sourceId:"preview-only", note:"根据合格放电测试外推；仅供容量参考。"})
    property variant reading: ({live:false, lastText:"2026-10-04 18:30", socText:"0%", currentText:"−450 mA", voltageText:"3.8 V", temperatureText:"28 °C"})
    property variant detail: ({id:"preview-only", battery_label:"主用电池", stateText:"已完成", status:"completed", capacityText:"约 1,850 mAh", estimateNote:"SOC 变化 40 个百分点，覆盖率 100%。", modeText:"放电估计", endText:"2026-10-04 18:30", mahText:"740 mAh", mwhText:"2,812 mWh", socText:"80% → 40%", elapsedText:"1 小时 40 分钟", coverageText:"100%", reasonText:"手动结束并保存", startText:"2026-10-04 16:50", meanCurrentText:"444 mA", gapsText:"0"})
    property variant test: ({modeText:"放电测试", remainingText:"23 分钟", mahText:"230 mAh", socText:"80% → 68%", elapsedText:"37 分钟", coverageText:"100%"})
    property variant batteries: [{key:"preview-a", label:"主用电池", active:true, testing:false, capacity:capacity}, {key:"preview-b",label:"备用电池",active:false,testing:false,capacity:{available:true,valueText:"1,620",sourceType:"charge-test",sourceText:"充电估计",timeText:"2026-10-03 12:00"}}, {key:"preview-c",label:"待测电池",active:false,testing:false,capacity:{available:false}}]
    property variant batteryModel: ArrayDataModel { }
    property variant results: ArrayDataModel { }
    Component.onCompleted: { batteryModel.append(batteries); }
    function refresh() {}
    function viewBattery(key) {}
    function selectTest(key) {}
    function exportData(all) {}
    function captureScreen() {}
    function loadMore() {}
    function useBattery(key) {}
    function renameBattery(key, label) {return true;}
    function createBattery(label) {return "preview-only";}
    function startTest(minutes, interval) {}
    function stopTest() {}
    function connectCollector() {return true;}
}
'''


def prepare(folder, scenario, overrides=None):
    assets = folder / 'assets'
    assets.mkdir(parents=True, exist_ok=True)
    for source in (ROOT / 'assets').glob('*.qml'):
        content = source.read_text(encoding='utf-8-sig')
        content = (overrides or {}).get(source.name, content)
        if source.name in PAGES:
            content = re.sub(r'(\n(?:NavigationPane|Page)\s*\{)', r'\1\n    property variant backend: PreviewBackend { }', content, count=1)
        content = content.replace('import bb.system 1.2', 'import "preview-system"')
        (assets / source.name).write_text(content, encoding='utf-8', newline='\n')
    shutil.copytree(ROOT / 'assets/icons', assets / 'icons', dirs_exist_ok=True)
    backend = BACKEND
    if scenario == 'empty':
        backend = backend.replace('batteryModel.append(batteries);', '')
        backend = re.sub(r'property variant batteries: .*', 'property variant batteries: []', backend)
        backend = backend.replace('property bool ready: true', 'property bool ready: false').replace('采集器就绪 · 待机不采样', '采集器尚未就绪，请进入测试页重新连接。')
    elif scenario == 'long-name':
        backend = backend.replace('主用电池', '这是用于检查长名称换行和操作区的四十字电池名称备用电池第一块')
    elif scenario == 'running':
        backend = backend.replace('property bool running: false', 'property bool running: true').replace('testing:false', 'testing:true', 1)
    elif scenario == 'pending':
        backend = backend.replace('property bool pending: false', 'property bool pending: true').replace('采集器就绪 · 待机不采样', '测试操作待确认，请保留原操作并等待核对。')
    (assets / 'PreviewBackend.qml').write_text(backend, encoding='utf-8', newline='\n')
    stubs = assets / 'preview-system'
    stubs.mkdir(exist_ok=True)
    (stubs / 'SystemToast.qml').write_text('import QtQuick 1.0\nQtObject { property string body; function show() {} }\n', encoding='utf-8')
    (stubs / 'PreviewButton.qml').write_text('import QtQuick 1.0\nQtObject { property string label }\n', encoding='utf-8')
    (stubs / 'PreviewInput.qml').write_text('import QtQuick 1.0\nQtObject { property string defaultText }\n', encoding='utf-8')
    (stubs / 'SystemPrompt.qml').write_text('''import QtQuick 1.0
QtObject {
    property string title
    property string body
    property PreviewInput inputField: PreviewInput { }
    property PreviewButton confirmButton: PreviewButton { }
    property PreviewButton cancelButton: PreviewButton { }
    signal finished(variant value)
    function show() {}
    function inputFieldTextEntry() {return inputField.defaultText;}
}
''', encoding='utf-8')
    return assets


def render(target, folder, page, scenario, seconds=4, overrides=None):
    folder = folder / uuid.uuid4().hex[:12]
    assets = prepare(folder, scenario, overrides)
    usr = target / 'win32/x86/usr'
    executable = usr / 'bin/qmlpreview.exe'
    env = dict(os.environ)
    env['PATH'] = str(usr / 'lib') + ';' + str(usr / 'lib/qt4/lib') + ';' + env['PATH']
    state = dict(file=(assets / page).as_uri(), device='Nevada', width=720, height=720,
                 scale=1, orientation='PORTRAIT', theme=1, themeString='dark', frameMarker=1)
    frames = folder / 'frames'
    frames.mkdir(exist_ok=True)
    args = [str(executable), '--assets', str(assets), '--system-wide-assets', str(target / 'qnx6/usr/share/assets'),
            '--target-path', str(target / 'qnx6'), '--frames-debug-dir', str(frames),
            '--no-dynamic-file-content', '--init-set-state', json.dumps(state)]
    with (folder / 'rpc.bin').open('wb') as out, (folder / 'stderr.txt').open('wb') as err:
        process = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=out, stderr=err)
        time.sleep(seconds)
        forced_stop = False
        if process.poll() is None:
            process.stdin.close()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                forced_stop = True
                process.kill()
        process.wait()
    log = (folder / 'stderr.txt').read_text(encoding='utf-8', errors='replace')
    errors = [line for line in log.splitlines() if ('QML error:' in line or 'ReferenceError:' in line or 'TypeError:' in line or 'Unable to assign' in line or re.search(r'\.qml:\d+:\d+:', line))]
    if not list(frames.iterdir()):
        errors.append('Native renderer did not produce a frame; validation incomplete')
    report = dict(page=page, scenario=scenario, sourceSha256=hashlib.sha256((ROOT / 'assets' / page).read_bytes()).hexdigest(),
                  previewExecutable=str(executable), systemPlugin='isolated stubs; device plugin not verified',
                  errors=errors, exitCode=process.returncode, forcedStop=forced_stop,
                  fixtureOverrides=bool(overrides), evidenceDirectory=str(folder),
                  files=[str(p) for p in frames.iterdir()])
    (folder / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=pathlib.Path, default=pathlib.Path(os.environ.get('INTROOP_SDK_ROOT', 'C:/bbdevtools')))
    parser.add_argument('--page', choices=PAGES, default='main.qml')
    parser.add_argument('--all', action='store_true', help='Load all production pages')
    parser.add_argument('--scenario', choices=('normal', 'empty', 'long-name', 'running', 'pending'), default='normal')
    args = parser.parse_args()
    targets = list(args.sdk.glob('target_*'))
    if len(targets) != 1:
        raise RuntimeError('Select SDK with one target directory')
    reports = [render(targets[0], ROOT / 'build/qml-preview' / (page + '-' + args.scenario), page, args.scenario)
               for page in (PAGES if args.all else (args.page,))]
    print(json.dumps(reports, ensure_ascii=True, indent=2))
    return 1 if any(report['errors'] for report in reports) else 0


if __name__ == '__main__':
    raise SystemExit(main())
