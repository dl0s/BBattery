"""Native Q10 migration acceptance; no synthetic samples are added to device history."""
import datetime
import json
import pathlib
import sqlite3
import sys
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device

checks = []
report = {"checks": checks, "result": "FAIL"}
screens = ROOT / "build/screenshots"


def check(name, condition):
    if not condition:
        raise AssertionError(name)
    checks.append(name)
    print("PASS " + name, flush=True)


def state():
    return device.app_call("state")

def settled():
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        value = state()
        if value["history"].get("loading") is False:
            return value
        time.sleep(0.3)
    raise AssertionError("History worker did not settle")


def capture(name):
    response = device.app_call("capture")
    path = response["path"]
    if path.startswith("file:"):
        path = urllib.parse.urlparse(path).path
    _, _, sandbox = device.receipt()
    assert path.startswith(sandbox + "/data/diagnostics/")
    device.scp(screens / (name + ".png"), path, True)
    check("Screenshot " + name, (screens / (name + ".png")).stat().st_size > 2000)


def test():
    original = state()["alerts"]
    try:
        for hours in (1, 6, 24, 168, 720):
            device.app_call("view", tab=1, hours=hours, follow=True, scroll=0)
            value = settled()
            history = value["history"]
            check("History scale " + str(hours), history["endMs"] - history["beginMs"] == hours * 3600000)
            check("Complete statistics " + str(hours), history["complete"])
            check("Bounded render points " + str(hours), history["displayPoints"] <= 4000)
            if hours == 720:
                check("Missing time is not full coverage", 0 < history["coverage"] < 95)
                capture("history-month")
        device.app_call("view", tab=1, hours=1, follow=True)
        latest = settled()["history"]["endMs"]
        device.app_call("view", tab=1, move=-1)
        older = settled()
        check("Previous window keeps collector running", not older["following"] and older["live"]["fresh"])
        check("Previous window moves one hour", abs(older["history"]["endMs"] - (latest - 3600000)) < 10000)
        device.app_call("view", tab=1, move=1)
        device.app_call("view", tab=1, follow=True)
        check("Return to latest", state()["following"])
        yesterday = datetime.datetime.now() - datetime.timedelta(days=1)
        device.app_call("view", tab=1, hours=24, date=int(yesterday.timestamp() * 1000), scroll=0)
        archived = settled()
        check("Date browsing is historical", not archived["following"])
        check("Empty history is explicit", archived["history"]["samples"] == 0)
        capture("history-empty")
        device.app_call("view", tab=1, hours=1, follow=True, parameter="current")
        settled()
        inspected = device.app_call("inspect", x=697, width=720)
        current = state()
        check("Raw sample inspection has values", "mA" in inspected["inspection"])
        check("SOC and parameter cursors synchronize",
              current["inspectionCursor"]["visible"] and
              current["inspectionCursor"] == inspected["cursor"])
        capture("history-inspection")
        device.app_call("view", tab=1, hours=720, follow=True)
        settled()
        no_sample = device.app_call("inspect", x=150, width=720)
        check("Missing interval is not fabricated", "此时无采样" in no_sample["inspection"])
        device.app_call("view", tab=1, hours=1, follow=True, showSoc=False, parameter="temperature", scroll=0)
        capture("history-temperature-only")
        device.app_call("view", tab=1, showSoc=True, parameter="current", display=True)
        capture("curve-settings")
        device.app_call("view", tab=1, close=True, calendar=True)
        capture("history-calendar")
        device.app_call("view", tab=1, close=True, scroll=10000)
        capture("history-summary")
        device.app_call("configure-alerts", enabled=False, low=25, high=95, temperature=46)
        changed = state()["alerts"]
        check("Alert settings save independently of sampling",
              changed == {"enabled": False, "low": 25, "high": 95, "temperature": 46} and state()["live"]["fresh"])
        device.stop_gui()
        device.launch()
        check("Alert settings remain typed and persisted after restart",
              state()["alerts"] == {"enabled": False, "low": 25, "high": 95, "temperature": 46})
        backup = device.app_call("backup")
        database = ROOT / "build/migration-history.sqlite"
        device.scp(database, backup["path"], True)
        with sqlite3.connect(database) as db, sqlite3.connect(ROOT / "build/pre-migration-history.sqlite") as previous:
            old = previous.execute("SELECT * FROM samples ORDER BY id").fetchall()
            preserved = db.execute("SELECT * FROM samples WHERE id<=? ORDER BY id", (old[-1][0],)).fetchall()
            check("All pre-migration samples are unchanged", old == preserved)
            check("Schema upgrade preserves integrity", db.execute("PRAGMA integrity_check").fetchone()[0] == "ok")
            charge_count = min(200, db.execute("SELECT COUNT(*) FROM sessions WHERE mode='charge'").fetchone()[0])
            report["database"] = {"samples": db.execute("SELECT COUNT(*) FROM samples").fetchone()[0],
                                  "preservedSamples": len(old)}
        device.app_call("view", tab=2, events=False, filter="charge")
        check("Filtered record count updates", state()["rows"] == charge_count)
        capture("records-filter")
        device.app_call("view", tab=2, events=True)
        capture("event-records")
        device.app_call("view", tab=3, scroll=600)
        capture("diagnostics-capacity")
        device.app_call("view", tab=3, scroll=1200)
        capture("diagnostics-history")
        final = state()
        check("Negative current verifies discharge", "放电" in final["quality"]["directionText"] and "尚无" not in final["quality"]["directionText"])
        check("PPS fallback recognizes no charger", final["live"]["chargerRaw"] == "NONE" and not final["live"]["externalPower"])
        check("System and battery current are separate",
              final["live"]["systemChargeText"] == "0 mA" and float(final["live"]["currentText"]) < 0)
        report["result"] = "PASS"
    finally:
        device.app_call("configure-alerts", **original)
        device.app_call("view", tab=0, close=True, hours=1, follow=True, parameter="current", showSoc=True, events=False, filter="")
        report["finalState"] = state()
        (ROOT / "build/migration-tests.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    test()
