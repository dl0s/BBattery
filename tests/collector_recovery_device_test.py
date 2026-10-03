"""Real-Q10 collector recovery and background sampling; does not reboot the device."""
import json
import pathlib
import re
import sqlite3
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device


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
    destination = device.BUILD / name
    device.scp(destination, response["path"], True)
    return destination


def test():
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
        check("repeated boot startup keeps the existing collector", pid == str(evidence["pid"]))
        supervisor = device.ssh("cat " + device.quote(device.SERVICE + "/supervisor.pid")).strip()
        assert supervisor.isdigit() and "bbattery-supervisor" in device.ssh("pidin -p " + supervisor + " ar")
        assert evidence["binary"] in device.ssh("pidin -p " + pid + " ar")
        device.ssh("kill -TERM " + supervisor + "\nkill -TERM " + pid)
        deadline = time.monotonic() + 20
        while ("bbattery-supervisor" in device.ssh("pidin -p " + supervisor + " ar", check=False)
               or evidence["binary"] in device.ssh("pidin -p " + pid + " ar", check=False)):
            assert time.monotonic() < deadline, "BBattery service did not stop"
            time.sleep(1)
        device.ssh(stanza.group(1))
        recovered = wait_state(lambda state: state["ready"] and state["collector"].get("run_id") != original["collector"].get("run_id"))
        new_pid = device.ssh("cat " + device.quote(pid_path)).strip()
        check("installed boot stanza recovers the stopped collector", new_pid.isdigit() and new_pid != pid)
        check("battery marker is preserved during service recovery", recovered["battery"] == original["battery"] and recovered["batteries"] == original["batteries"])
        idle = backup("collector-repair-idle.sqlite")
        with sqlite3.connect(before) as old, sqlite3.connect(idle) as current:
            check("idle service recovery creates no samples", old.execute("SELECT COUNT(*) FROM samples").fetchone() == current.execute("SELECT COUNT(*) FROM samples").fetchone())

        device.app_call("start-test", minutes=1, interval=10)
        running = wait_state(lambda state: state["running"] and not state["pending"])
        test_id = running["test"]["id"]
        check("real battery test starts with an immediate sample", running["test"]["samples"] >= 1 and running["test"]["battery_key"] == original["battery"]["activeKey"])
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
        (device.BUILD / "collector-repair-tests.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    test()
