"""Record current host checks; never treats them as device acceptance."""
import datetime
import json
import pathlib
import subprocess
import sys
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    folder = ROOT / 'build/host-checks' / uuid.uuid4().hex[:12]
    folder.mkdir(parents=True)
    commands = [['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'tools/test-capacity.ps1']]
    commands += [[sys.executable, '-B', 'tests/' + test] for test in
                 ('device_tools_test.py', 'collector_release_test.py', 'package_test.py', 'qml_load_test.py')]
    commands.append([sys.executable, '-B', 'tools/audit_bar.py', 'build/BBattery.bar'])
    reports = []
    for index, command in enumerate(commands):
        started = datetime.datetime.now(datetime.timezone.utc).isoformat()
        result = subprocess.run(command, cwd=ROOT, capture_output=True, encoding='utf-8', errors='replace', timeout=120)
        path = folder / ('check-' + str(index + 1) + '.log')
        path.write_text(result.stdout + result.stderr, encoding='utf-8', newline='\n')
        reports.append(dict(command=command, startedAt=started, exitCode=result.returncode, log=str(path)))
        print(('PASS ' if result.returncode == 0 else 'FAIL ') + command[-1], flush=True)
        if result.returncode:
            break
    report = dict(scope='host-only', result='PASS' if all(item['exitCode'] == 0 for item in reports) and len(reports) == len(commands) else 'FAIL', checks=reports)
    path = folder / 'report.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(str(path))
    return 0 if report['result'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
