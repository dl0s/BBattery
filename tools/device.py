"""BBmanager protocol v1 client: submit and observe, never execute device commands.

Authoritative workflow: BBmanager/Documentation/标准操作文档.md.
Connection snapshots come from verified endpoints; caller never owns device locks.
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CLI = pathlib.Path(os.environ.get("BBMANAGER_CLI", str(ROOT.parent / "BBmanager/dist/Q10Manager/cli/Q10Manager.Cli.exe")))
TERMINAL = {"succeeded", "failed", "cancelled", "unconfirmed", "resolved", "abandoned"}
READ_COMMANDS = {"status", "events", "result", "failure", "deployment-details", "build-logs", "devices", "inspect-bar"}


def cli(command, *arguments, store=None):
    if command not in READ_COMMANDS | {"submit"}:
        raise ValueError("BBattery only submits and reads protocol tasks: " + command)
    if not CLI.is_absolute() or not CLI.is_file():
        raise FileNotFoundError("Set BBMANAGER_CLI to the full published CLI path")
    args = [str(CLI), command, *map(str, arguments)]
    if store is not None:
        args.extend(["--store", str(pathlib.Path(store).resolve())])
    result = subprocess.run(args, capture_output=True, encoding="utf-8-sig", timeout=45)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip() or "BBmanager CLI failed")
    return result.stdout.strip()


def connection_profile(local_appdata=None):
    path = pathlib.Path(local_appdata or os.environ["LOCALAPPDATA"]) / "Q10Manager/connection.json"
    if not path.is_file():
        raise FileNotFoundError("Configure and register the device in BBmanager; no legacy fallback")
    return path


def connection(host=None, profile=None):
    if host:
        matches = [endpoint for device in json.loads(cli("devices")) for endpoint in device["endpoints"]
                   if endpoint["deviceHost"].lower() == host.lower()]
        if len(matches) != 1:
            raise ValueError("Target needs a unique verified BBmanager endpoint; register it first")
        return matches[0]
    original = json.loads(pathlib.Path(profile or connection_profile()).read_text(encoding="utf-8-sig"))
    snapshot = {name[0].lower() + name[1:]: value for name, value in original.items()}
    if not snapshot.get("expectedPin") or not snapshot.get("hostPublicKey"):
        raise ValueError("Ordinary operations require a registered PIN and trusted host identity")
    return snapshot


def save(path, record, exclusive=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    if exclusive:
        with path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(record, stream, ensure_ascii=False, indent=2)
            stream.flush()
            os.fsync(stream.fileno())
        return
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".next")
    with temporary.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(record, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def submit(request, intent=None, store=None):
    """Persist stable ID and exact request BEFORE invoking submit, including ambiguity.

    Restarting an existing intent always reads the original ID. An ambiguous submission
    is never repaired by sending it again, even if a later lookup reports missing.
    """
    request = dict(request, protocolVersion=1, client="BBattery")
    if intent is None:
        intent = BUILD / "protocol" / (uuid.uuid4().hex + ".json")
    intent = pathlib.Path(intent)
    if intent.exists():
        previous = json.loads(intent.read_text(encoding="utf-8"))
        if previous["store"] != (str(store) if store else "default-shared-v1"):
            raise ValueError("Original protocol store differs")
        if previous["request"] != dict(request, id=previous["id"]):
            raise ValueError("Original intent parameters differ; preserve the original task")
        cli("status", previous["id"], store=store)
        return previous["id"]
    request = dict(request, protocolVersion=1, client="BBattery", id=request.get("id", uuid.uuid4().hex))
    record = {"id": request["id"], "request": request, "phase": "submission-may-have-started", "store": str(store) if store else "default-shared-v1"}
    save(intent, record, exclusive=True)
    request_path = intent.with_name(intent.stem + "-request.json")
    save(request_path, request, exclusive=True)
    try:
        accepted = cli("submit", request_path, store=store)
        if accepted != request["id"] or not re.fullmatch("[0-9a-f]{32}", accepted):
            raise RuntimeError("Submission response differs; query the original saved ID")
        record["phase"] = "submitted"
        save(intent, record)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        raise RuntimeError("Submission uncertain; query original ID " + request["id"] + "; intent " + str(intent)) from error
    return request["id"]


def observe(task_id, store=None):
    if not re.fullmatch("[0-9a-f]{32}", task_id):
        raise ValueError("Invalid protocol task ID")
    entry = json.loads(cli("status", task_id, store=store))
    output = {"id": task_id, "state": entry["state"]}
    if entry["state"]["status"] == "succeeded":
        output["result"] = json.loads(cli("result", task_id, store=store))
    elif entry["state"]["status"] in TERMINAL:
        output["events"] = json.loads(cli("events", task_id, store=store))
        try:
            output["failure"] = json.loads(cli("failure", task_id, store=store))
        except RuntimeError as error:
            output["failureReadError"] = str(error)
    return output


def wait(task_id, seconds=45, store=None):
    deadline = time.monotonic() + min(max(seconds, 0), 60)
    while True:
        result = observe(task_id, store)
        if result["state"]["status"] in TERMINAL or time.monotonic() >= deadline:
            return result
        time.sleep(1)


def install(host=None, profile=None, bar=None):
    package = pathlib.Path(bar or BUILD / "BBattery.bar").resolve()
    audit = json.loads(cli("inspect-bar", package))
    if audit.get("issues"):
        raise ValueError("BAR audit failed")
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    pinned = BUILD / "releases" / digest / "BBattery.bar"
    pinned.parent.mkdir(parents=True, exist_ok=True)
    if pinned.exists() and hashlib.sha256(pinned.read_bytes()).hexdigest() != digest:
        raise ValueError("Preserved package differs")
    if not pinned.exists():
        shutil.copyfile(package, pinned)
    snapshot = connection(host, profile)
    request = {"protocolVersion": 1, "client": "BBattery", "kind": "deploy", "connection": snapshot,
               "packagePath": str(pinned), "packageSha256": digest}
    # Multiple devices have independent intents even when the BAR and host key match.
    intent = BUILD / "protocol" / ("deploy-" + snapshot["expectedPin"] + "-" + digest + ".json")
    return submit(request, intent)


def diagnose(kind, host=None, profile=None, application=None):
    if kind not in {"information", "applications", "installerStatus", "installerDiagnostics", "processes", "applicationLog"}:
        raise ValueError("Unsupported fixed diagnostic")
    request = {"kind": kind, "connection": connection(host, profile)}
    if application:
        request["applicationDirectory"] = application
    return submit(request)


def list_files(remote_path, host=None, profile=None):
    return submit({"kind": "deviceFiles", "connection": connection(host, profile), "fileAction": "list", "remotePath": remote_path})


def download_file(entry, local_path, host=None, profile=None):
    local_path = pathlib.Path(local_path).resolve()
    local_path.parent.mkdir(parents=True, exist_ok=True)
    return submit({"kind": "deviceFiles", "connection": connection(host, profile), "fileAction": "download",
                   "remotePath": entry["path"], "expectedFileIdentity": entry["identity"], "localPath": str(local_path)})


def continue_deployment(original_id, intent=None):
    entry = json.loads(cli("status", original_id))
    original = entry["request"]
    if original["kind"] != "deploy":
        raise ValueError("Original task is not a BAR deployment")
    request = {"protocolVersion": 1, "client": "BBattery", "kind": "deploymentObservation",
               "connection": original["connection"], "originalDeploymentId": original_id,
               "packageSha256": original["packageSha256"]}
    return submit(request, intent)


def unsupported(*args, **kwargs):
    raise RuntimeError("Current BBmanager protocol has no BBattery collector/lifecycle/private-GUI operation. "
                       "Preserve startup chain; no legacy maintenance or direct transport fallback.")


# Fail closed for historical test callers; these names never open a connection.
ssh = scp = app_call = launch = stop_gui = provision = install_boot_hook = unsupported


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("install", "diagnose", "status", "continue-deployment", "build", "provision", "boot-hook", "launch", "recover", "stop-gui", "state"))
    parser.add_argument("value", nargs="?")
    parser.add_argument("--host")
    parser.add_argument("--connection", type=pathlib.Path)
    parser.add_argument("--bar", type=pathlib.Path)
    parser.add_argument("--intent", type=pathlib.Path)
    args = parser.parse_args()
    if args.command == "install":
        output = install(args.host, args.connection, args.bar)
    elif args.command == "diagnose":
        output = diagnose(args.value or "information", args.host, args.connection)
    elif args.command == "status":
        output = observe(args.value)
    elif args.command == "continue-deployment":
        output = continue_deployment(args.value, args.intent)
    elif args.command == "build":
        output = submit({"kind": "build", "projectDirectory": str(ROOT), "sdkDirectory": str(pathlib.Path(os.environ["INTROOP_SDK_ROOT"]).resolve()),
                         "buildScriptPath": str(ROOT / "build.ps1"), "buildOutputPath": str(BUILD / "BBattery.bar"), "buildSwitches": ["Package"]}, args.intent)
    else:
        unsupported()
    print(json.dumps(output, ensure_ascii=False, indent=2) if isinstance(output, dict) else output)


if __name__ == "__main__":
    main()
