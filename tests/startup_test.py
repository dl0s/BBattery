"""Repeated real-device launches and chart switches for the startup crash."""
import json
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import device


def test():
    evidence = {"result": "FAIL", "launches": []}
    try:
        receipt, _, sandbox = device.receipt()
        original = device.app_call("state")
        for attempt, parameter in enumerate(("current", "voltage", "temperature"), 1):
            device.stop_gui()
            device.launch()
            time.sleep(2)
            state = device.app_call("state")
            pids = device.gui_pids()
            assert len(pids) == 1, "Expected exactly one GUI process"
            assert state["euid"] != 0 and state["live"]["fresh"], "GUI or collector unavailable"
            assert state["alerts"] == original["alerts"], "Startup changed persisted alert settings"
            assert state["live"]["paused"] == original["live"]["paused"], "Startup changed sampling settings"
            logs = device.ssh("cat " + device.quote(sandbox + "/logs/log"))
            assert "BBattery " + receipt["version"] + " native scene ready" in logs
            assert "SIGSEGV" not in logs, "Launch produced a segmentation fault"
            device.app_call("view", tab=1, parameter=parameter, hours=6 if attempt == 2 else 1)
            assert device.app_call("state")["live"]["fresh"], "Chart switch interrupted the GUI"
            evidence["launches"].append({"attempt": attempt, "pid": pids[0],
                                        "parameter": parameter, "samples": state["quality"]["samples"]})
            print("PASS startup " + str(attempt) + " and " + parameter + " chart", flush=True)
        device.app_call("view", tab=0, hours=1, parameter="current")
        evidence["result"] = "PASS"
    finally:
        (ROOT / "build/startup-tests.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")


if __name__ == "__main__":
    test()
