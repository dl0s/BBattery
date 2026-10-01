"""Raw-sample inspection and bounded drag work on the real Q10."""
import argparse
import json
import pathlib
import sqlite3
import sys
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device


def settled():
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        value = device.app_call("state")
        if value["history"].get("loading") is False:
            return value
        time.sleep(0.3)
    raise AssertionError("History did not settle")

def capture(name):
    response = device.app_call("capture")
    path = response["path"]
    if path.startswith("file:"):
        path = urllib.parse.urlparse(path).path
    _, _, sandbox = device.receipt()
    assert path.startswith(sandbox + "/data/diagnostics/")
    destination = ROOT / ("build/screenshots/" + name + ".png")
    destination.parent.mkdir(parents=True, exist_ok=True)
    device.scp(destination, path, True)
    assert destination.stat().st_size > 2000


def test_raw_neighbours():
    backup = device.app_call("backup")
    database = ROOT / "build/inspection-history.sqlite"
    device.scp(database, backup["path"], True)
    device.app_call("view", tab=1, hours=1, follow=True)
    settled()
    device.app_call("view", tab=1, move=-1)
    history = settled()["history"]
    begin, end = history["beginMs"], history["endMs"]
    with sqlite3.connect(database) as db:
        rows = db.execute("SELECT utc_ms FROM samples WHERE utc_ms>=? AND utc_ms<=? "
                          "ORDER BY utc_ms,id", (begin, end)).fetchall()
    pairs = [(a[0], b[0]) for a, b in zip(rows, rows[1:]) if 0 < b[0] - a[0] <= 10000]
    assert pairs, "No real adjacent samples available"
    before, after = pairs[len(pairs) // 2]
    for fraction, expected in ((0.25, before), (0.75, after)):
        target = before + (after - before) * fraction
        x = 76 + 624 * (target - begin) / (end - begin)
        result = device.app_call("inspect", x=x, width=720)
        actual = round(begin + result["cursor"]["ratio"] * (end - begin))
        assert actual == expected, "Cached neighbours selected the wrong raw sample"
        assert "mA" in result["inspection"]
    print("PASS raw-neighbour selection on both sides of the midpoint", flush=True)


def test(baseline=False):
    report = {"result": "FAIL", "benchmarks": []}
    try:
        for hours in (1, 720):
            device.app_call("view", tab=1, hours=hours, follow=True, parameter="current", scroll=0)
            settled()
            result = device.app_call("inspect-benchmark", count=120)
            report["benchmarks"].append(dict(hours=hours, **result))
            if baseline:
                assert result["chartRenders"] == 240
            else:
                assert result["chartRenders"] == 0, "Drag regenerated chart images"
                assert result["elapsedMs"] < 100, "Touch handler blocked the UI"
                assert result["settleMs"] < 1000, "Final drag target did not settle"
                assert result["lookupCount"] <= 2, "Obsolete drag requests were not coalesced"
                assert result["cursor"]["ratio"] > 0.99, "Final drag position was lost"
            print("PASS " + str(hours) + "h drag: " + str(result["elapsedMs"]) +
                  "ms, " + str(result["chartRenders"]) + " chart renders", flush=True)
        if not baseline:
            test_raw_neighbours()
            device.app_call("view", tab=1, hours=1, follow=True)
            value = settled()
            capture("inspection-before")
            inspection = device.app_call("inspect", x=697, width=720)
            assert "mA" in inspection["inspection"], "Latest raw sample unavailable"
            assert inspection["cursor"]["visible"], "Inspection cursor hidden"
            value = device.app_call("state")
            assert value["inspectionCursor"] == inspection["cursor"]
            capture("inspection-after")
            device.app_call("view", tab=1, hours=720, follow=True)
            settled()
            missing = device.app_call("inspect", x=150, width=720)
            assert "此时无采样" in missing["inspection"], "Gap fabricated a reading"
            device.app_call("view", tab=1, hours=6)
            value = settled()
            assert not value["inspectionCursor"]["visible"], "Range switch retained stale cursor"
            assert value["inspection"] == ""
            print("PASS raw samples, gaps, shared cursor and range reset", flush=True)
        report["result"] = "PASS"
    finally:
        device.app_call("view", tab=1, hours=1, follow=True, parameter="current", scroll=0)
        suffix = "baseline" if baseline else "tests"
        (ROOT / ("build/inspection-" + suffix + ".json")).write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", action="store_true")
    test(parser.parse_args().baseline)
