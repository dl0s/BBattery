"""Real-Q10 collector recovery and background sampling; does not reboot the device."""
import json
import pathlib
import re
import sqlite3
import sys
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device
import collector_release

EVIDENCE = device.BUILD / ('acceptance-0.1.0.9-' + uuid.uuid4().hex[:12])
EVIDENCE.mkdir(parents=True, exist_ok=False)


def wait_state(predicate, timeout=35):
    deadline = time.monotonic() + timeout
    while True:
        device.app_call("refresh")
        state = device.app_call("state")
        if predicate(state):
            return state
        if time.monotonic() >= deadline:
            raise AssertionError("Collector/UI confirmation timed out: " + json.dumps(state, ensure_ascii=False))
        time.sleep(1)


def backup(name):
    response = device.app_call("backup")
    destination = EVIDENCE / name
    device.scp(destination, response["path"], True)
    return destination


def business_checks(unsigned_before):
    report = {"result": "FAIL", "checks": [], "actualDeviceReboot": False}
    test_id = None
    original = wait_state(lambda state: state["ready"] and not state["refreshing"])
    assert not original["running"], "Run only when no user test is active"

    def check(label, condition):
        assert condition, label
        report["checks"].append(label)
        print("PASS " + label, flush=True)

    try:
        before = backup("collector-repair-before.sqlite")
        evidence = json.loads((device.BUILD / "collector-evidence.json").read_text(encoding="utf-8"))
        _, _, native = device.installed_collector(str(evidence['uid']), str(evidence['gid']))
        (EVIDENCE / 'native-tests.txt').write_text(native, encoding='utf-8')
        check("packaged native durability, readiness and storage self-tests pass", "FAIL:" not in native)
        check("independent collector and GUI use the sandbox UID", evidence["uid"] == original["euid"] != 0)
        check("service configuration is stored under the persistent account", evidence["serviceDirectory"] == device.SERVICE)
        boot = device.ssh("cat " + device.quote(evidence["bootHook"]))
        stanza = re.search(r"^# BBATTERY COLLECTOR BOOT BEGIN\n(.*?)^# BBATTERY COLLECTOR BOOT END", boot, re.M | re.S)
        assert stanza, "Installed BBattery boot stanza missing"
        # Execute only BBattery's installed startup stanza, never the other boot services.
        device.ssh(stanza.group(1))
        device.ssh(stanza.group(1))
        _, _, sandbox = device.receipt()
        pid_path = sandbox + "/data/battery/collector.pid"
        pid = device.ssh("cat " + device.quote(pid_path)).strip()
        check("repeated boot startup keeps the existing collector", pid == str(original["collector"]["pid"]))
        check("bounded startup leaves no resident BBattery supervisor", "bbattery-supervisor" not in device.ssh("pidin ar"))
        assert evidence["binary"] in device.ssh("pidin -p " + pid + " ar")
        lock_path = sandbox + '/data/battery/collector.lock'
        lock_identity = device.ssh('ls -i ' + device.quote(lock_path)).strip()
        duplicate = 'on -u {}:{},1000 {} --data {} 2>&1\ncode=$?\nprint "duplicate_exit=$code"'.format(evidence['uid'],evidence['gid'],evidence['binary'],sandbox+'/data/battery')
        check("duplicate collector is rejected by the real kernel lock", 'duplicate_exit=4' in device.ssh(duplicate))
        device.ssh("kill -TERM " + pid)
        deadline = time.monotonic() + 20
        while evidence["binary"] in device.ssh("pidin -p " + pid + " ar", check=False):
            assert time.monotonic() < deadline, "BBattery service did not stop"
            time.sleep(1)
        stopped = wait_state(lambda state: not state['ready'])
        check("stopped process cannot remain ready through its recent heartbeat", not stopped['ready'])
        device.ssh(stanza.group(1))
        recovered = wait_state(lambda state: state["ready"] and state["collector"].get("run_id") != original["collector"].get("run_id"))
        new_pid = device.ssh("cat " + device.quote(pid_path)).strip()
        check("installed boot stanza recovers the stopped collector", new_pid.isdigit() and new_pid != pid)
        check("recovery retains the persistent lock inode", device.ssh('ls -i '+device.quote(lock_path)).strip()==lock_identity)
        check("battery marker is preserved during service recovery", recovered["battery"] == original["battery"] and recovered["batteries"] == original["batteries"])
        idle = backup("collector-repair-idle.sqlite")
        with sqlite3.connect(before) as old, sqlite3.connect(idle) as current:
            check("idle service recovery creates no samples", old.execute("SELECT COUNT(*) FROM samples").fetchone() == current.execute("SELECT COUNT(*) FROM samples").fetchone())

        # Pause only the verified idle collector to exercise durable GUI intent.
        device.ssh('kill -STOP ' + new_pid)
        try:
            device.app_call("start-test", minutes=1, interval=10)
            pending = device.app_call('state')
            check("unconfirmed test remains pending", pending['pending'])
            device.stop_gui()
            device.launch()
            pending = device.app_call('state')
            check("GUI restart reloads the original pending operation", pending['pending'])
            try:
                device.app_call('start-test',minutes=1,interval=10)
                raise AssertionError('A pending test allowed a replacement command')
            except RuntimeError as error:
                assert 'GUI operation failed' in str(error)
                check('pending operation refuses replacement submission', True)
        finally:
            device.ssh('kill -CONT ' + new_pid)
        confirmed = wait_state(lambda state: not state['pending'])
        if not confirmed['running']:
            check('expired original operation confirms failure and restores buttons', '\u8fc7\u671f' in confirmed['status'])
            device.app_call('start-test',minutes=1,interval=10)
        running = wait_state(lambda state: state["running"] and not state["pending"])
        test_id = running["test"]["id"]
        check("real battery test starts with an immediate sample", running["test"]["samples"] >= 1 and running["test"]["battery_key"] == original["battery"]["activeKey"])
        device.ssh('kill -HUP '+new_pid+'\nkill -PIPE '+new_pid)
        check('observer hangup and closed-output signal keep the actual collector alive',evidence['binary'] in device.ssh('pidin -p '+new_pid+' ar'))
        device.stop_gui()
        print("Checking the one-minute test while the GUI is closed...", flush=True)
        time.sleep(35)
        print("Collector continues with GUI closed; waiting for the deadline...", flush=True)
        time.sleep(25)
        device.launch()
        completed = wait_state(lambda state: not state["running"] and state["test"].get("id") == test_id)
        report["test"] = completed["test"]
        check("test ends and saves with the GUI closed", completed["test"]["status"] == "completed" and completed["test"]["reason"] == "timer" and completed["test"]["elapsed_s"] == 60)
        check("real readings are sampled repeatedly", completed["test"]["samples"] >= 4 and completed["test"]["integrated_s"] > 0)
        check("short SOC range does not fabricate battery capacity", not completed["test"]["estimateReady"])
        final = backup("collector-repair-after.sqlite")
        baseline = EVIDENCE / 'deployment-baseline.sqlite'
        device.scp(baseline,device.SERVICE+'/updates/'+evidence['deployment']+'/history.sqlite',True)
        with sqlite3.connect(baseline) as old, sqlite3.connect(before) as current:
            for table in ('samples','sessions','snapshots','session_batteries','battery_profiles','capacity_tests'):
                column=old.execute('PRAGMA table_info('+table+')').fetchall()[0][1]
                for row in old.execute('SELECT * FROM '+table).fetchall():
                    saved=current.execute('SELECT * FROM '+table+' WHERE '+column+'=?',(row[0],)).fetchone()
                    if table=='battery_profiles':
                        labels={item['key']:item['label'] for item in original['batteries']}
                        assert saved==(row[0],labels.get(row[0],row[1]))
                    else:
                        assert saved==row
            check('pre-activation history and battery identities retain the current user labels',True)
        with sqlite3.connect(before) as old, sqlite3.connect(final) as current:
            check("SQLite integrity and foreign keys are valid", current.execute("PRAGMA integrity_check").fetchone()[0] == "ok" and not current.execute("PRAGMA foreign_key_check").fetchall())
            for table in ("samples", "sessions", "snapshots", "session_batteries", "battery_profiles", "capacity_tests"):
                rows = old.execute("SELECT * FROM " + table + " ORDER BY 1").fetchall()
                # IDs are not chronological for tests; compare each existing row by primary key.
                for row in rows:
                    column = old.execute("PRAGMA table_info(" + table + ")").fetchall()[0][1]
                    assert current.execute("SELECT * FROM " + table + " WHERE " + column + "=?", (row[0],)).fetchone() == row
                check("existing " + table + " rows are retained", True)
            count = current.execute("SELECT COUNT(*) FROM test_samples WHERE test_id=?", (test_id,)).fetchone()[0]
            check("samples belong to the saved test", count == completed["test"]["samples"])
        check("only one GUI instance is running", len(device.gui_pids()) == 1)
        evidence["pid"] = int(new_pid)
        evidence["bootStartupRecoveryVerified"] = True
        evidence["identity"] = device.ssh("pidin -p " + new_pid + " users")
        (device.BUILD / "collector-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
        report["result"] = "PASS"
    finally:
        device.launch()
        final_state = device.app_call("state")
        if test_id and final_state["running"] and final_state["test"].get("id") == test_id:
            device.app_call("stop-test")
            wait_state(lambda state: not state["running"] and not state["pending"])
        report['evidenceDirectory']=str(EVIDENCE)
        (EVIDENCE / "collector-repair-tests.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        print('Acceptance evidence: '+str(EVIDENCE),flush=True)
    return report


def test():
    before=collector_release.observe()
    with device.lease():
        report=business_checks(before)
    try:
        after=collector_release.observe()
        assert before['frozenHashes']==after['frozenHashes'] and before['unsigned']['service_pid']==after['unsigned']['service_pid'] and not after['unfinished']
        report['checks'].append('frozen unsigned files and native consumer remain unchanged')
        report['unsignedBefore']=before['unsigned'];report['unsignedAfter']=after['unsigned']
        report['result']='PASS'
    except Exception:
        report['result']='FAIL';raise
    finally:
        (EVIDENCE/'collector-repair-tests.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')


if __name__ == "__main__":
    test()
