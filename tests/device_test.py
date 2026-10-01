"""Real Q10 acceptance. No fabricated battery records and no unrelated process control."""
import argparse
import datetime
import json
import pathlib
import re
import sqlite3
import sys
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device

results = []
report = dict(startedAt=datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),
              timezone="Asia/Shanghai", checks=results, coldBoot="NOT_TESTED",
              actualFullDischargeCapacity="NOT_TESTED")
screens = ROOT / "build/screenshots"
screens.mkdir(exist_ok=True)


def check(label, condition):
    if not condition:
        raise AssertionError(label)
    results.append(label)
    print("PASS " + label, flush=True)


def capture(name):
    response = device.app_call("capture")
    path = response["path"]
    if path.startswith("file:"):
        path = urllib.parse.urlparse(path).path
    _, _, sandbox = device.receipt()
    if not path.startswith(sandbox + "/data/diagnostics/"):
        raise RuntimeError("Screenshot escaped the application's own diagnostics directory")
    device.scp(screens / (name + ".png"), path, True)
    check("Own-window screenshot: " + name, (screens / (name + ".png")).stat().st_size > 2000)


def state():
    return device.app_call("state")


def test(smoke):
    _, _, sandbox = device.receipt()
    logs = device.ssh("tail -n 100 " + device.quote(sandbox + "/logs/log"))
    check("Actual native QML scene loaded", "BBattery 0.1.0." in logs and "native scene ready" in logs)
    before = state()
    check("GUI remains non-root", before["euid"] != 0)
    check("Collector is alive", before["live"]["fresh"])
    check("Observed data exists", before["quality"]["samples"] > 0)
    check("Average current is explicitly represented", before["live"]["currentText"] != "--")
    check("Missing capacity is not displayed as zero", before["live"]["fullText"] != "0 mAh")
    report["initialState"] = before
    for i, name in enumerate(("overview", "trends", "records", "quality")):
        response = device.app_call("view", tab=i)
        check("Native tab navigates: " + name, response["layout"]["tab"] == i)
        time.sleep(0.6)
        capture(name)
    device.app_call("view", tab=0, settings=True)
    time.sleep(0.5)
    capture("settings")
    device.app_call("view", tab=0, close=True)
    plot = device.app_call("chart-export")
    device.scp(screens / "soc-plot.png", plot["soc"], True)
    device.scp(screens / "parameter-plot.png", plot["parameter"], True)
    check("Data-driven chart images exported", (screens / "soc-plot.png").stat().st_size > 1000)
    if smoke:
        return
    initial_count = state()["quality"]["samples"]
    device.stop_gui()
    time.sleep(23)
    device.launch()
    time.sleep(3)
    continued = state()
    check("Samples continue with GUI closed", continued["quality"]["samples"] >= initial_count + 2)
    check("Collector still alive after GUI restart", continued["live"]["fresh"])

    device.app_call("configure", interval=5, paused=True)
    time.sleep(7)
    paused = state()
    check("Pause acknowledged by collector", paused["live"]["paused"])
    count = paused["quality"]["samples"]
    time.sleep(11)
    check("Pause creates no synthetic samples", state()["quality"]["samples"] == count)
    device.app_call("configure", interval=10, paused=False)
    time.sleep(8)
    resumed = state()
    check("Continuous sampling resumes", not resumed["live"]["paused"] and resumed["quality"]["samples"] > count)

    evidence = json.loads((ROOT / "build/collector-evidence.json").read_text())
    directory = evidence["data"]
    old_pid = device.ssh("cat " + device.quote(directory + "/collector.pid")).strip()
    command = device.ssh("pidin -p " + old_pid + " ar")
    check("Recovery target is only this collector", old_pid.isdigit() and evidence["binary"] in command)
    old_gaps = resumed["quality"]["gapCount"]
    device.ssh("kill -TERM " + old_pid)
    deadline = time.monotonic() + 25
    new_pid = old_pid
    while time.monotonic() < deadline:
        new_pid = device.ssh("cat " + device.quote(directory + "/collector.pid"), check=False).strip()
        if new_pid.isdigit() and new_pid != old_pid and evidence["binary"] in device.ssh("pidin -p " + new_pid + " ar", check=False):
            break
        time.sleep(1)
    check("Local supervisor restores a stopped collector", new_pid != old_pid)
    time.sleep(3)
    recovered = state()
    check("Recovery records a discontinuity", recovered["quality"]["gapCount"] > old_gaps)
    check("Recovery preserves history", recovered["quality"]["samples"] >= resumed["quality"]["samples"])
    identity = device.ssh("pidin -p " + new_pid + " users")
    check("Restored collector is non-root", re.search(r"\s100\s+\d+\s+100\s+\d+\s", identity) is not None)

    backup = device.app_call("backup")
    database = ROOT / "build/device-history.sqlite"
    device.scp(database, backup["path"], True)
    with sqlite3.connect(database) as db:
        check("SQLite integrity", db.execute("PRAGMA integrity_check").fetchone()[0] == "ok")
        check("Sentinels never become measurement values", db.execute(
            "SELECT COUNT(*) FROM samples WHERE current=80000000 OR full_capacity=80000000 OR voltage=80000000").fetchone()[0] == 0)
        check("No unsupported full capacity fabricated", db.execute(
            "SELECT COUNT(*) FROM samples WHERE full_capacity IS NOT NULL").fetchone()[0] == 0)
        check("Raw snapshots retained", db.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0] > 0)
        check("Pause and restart boundaries retained", db.execute(
            "SELECT COUNT(*) FROM samples WHERE gap_reason IN ('paused','collector-restart')").fetchone()[0] >= 2)
        check("Private history includes multiple sessions", db.execute("SELECT COUNT(*) FROM sessions").fetchone()[0] >= 3)
        report["database"] = dict(samples=db.execute("SELECT COUNT(*) FROM samples").fetchone()[0],
                                  sessions=db.execute("SELECT COUNT(*) FROM sessions").fetchone()[0],
                                  snapshots=db.execute("SELECT COUNT(*) FROM snapshots").fetchone()[0])
        recent_id = db.execute("SELECT id FROM sessions ORDER BY id DESC LIMIT 1").fetchone()[0]
    detail = device.app_call("select-session", id=recent_id)["detail"]
    check("Live session refuses a capacity conclusion", not detail["estimateReady"])
    device.app_call("view", tab=2, session=True)
    time.sleep(0.5)
    capture("session-detail")
    device.app_call("view", tab=0, close=True)
    device.app_call("export")
    listing = device.ssh("ls " + device.quote(sandbox + "/shared/documents/BBattery"))
    check("CSV and quality report exported", "-samples.csv" in listing and "-quality.json" in listing)
    report["finalState"] = state()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    try:
        test(args.smoke)
        report["result"] = "PASS"
    except Exception as error:
        report["result"] = "FAIL"
        report["failure"] = str(error)
        raise
    finally:
        (ROOT / "build/device-tests.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
