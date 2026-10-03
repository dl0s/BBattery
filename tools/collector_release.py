"""BBattery releases through the frozen Q10 maintenance coordinator."""
import base64
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import time
import uuid
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
COORDINATOR = ROOT.parent / "Autoloader/q10_analysis/application_unsigned/with_device_lock.ps1"
EVIDENCE = ROOT.parent / "Autoloader/q10_analysis/live_192.168.1.61_20261003"
SERVICE = "/accounts/1000/bbattery-service"


def digest(body):
    return hashlib.sha256(body).hexdigest()


def package(path):
    body = path.read_bytes()
    with zipfile.ZipFile(path) as archive:
        raw = archive.read("META-INF/MANIFEST.MF").decode("utf-8").replace("\r\n ", "").replace("\n ", "")
        fields = dict(line.split(": ", 1) for line in raw.splitlines() if ": " in line)
        if fields.get("Package-Name") != "top.blaccat.BBattery":
            raise ValueError("Release must be the BBattery application")
        assets = {name: digest(archive.read(name)) for name in archive.namelist()
                  if name.startswith("native/") and not name.endswith("/")}
    if len(body) > 8 * 1024 * 1024:
        raise ValueError("BBattery BAR exceeds the 8 MiB deployment limit")
    return {"sha256": digest(body), "version": fields["Package-Version"], "assets": assets,
            "base64": base64.b64encode(body).decode("ascii")}


def run(action, plan):
    """Each observation gets new evidence; the deployment identity stays unchanged."""
    invocation = uuid.uuid4().hex[:12]
    label = plan["tag"] + "-" + action + "-" + invocation
    work = ROOT / "build/deployments" / plan["tag"]
    work.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "tools/collector_maintenance.py").read_text(encoding="utf-8")
    remote_plan = json.loads(json.dumps(plan))
    if action != 'observe':
        stage_payloads(plan)
        for field in ('old', 'new'):
            remote_plan[field].pop('base64')
    encoded = json.dumps(dict(remote_plan, action=action), ensure_ascii=True, sort_keys=True)
    # Legacy ksh heredoc parsing must not receive a multi-megabyte single line.
    chunks = [repr(encoded[start:start + 1024]) for start in range(0, len(encoded), 1024)]
    source = "import json\nPLAN=json.loads(\n" + "\n".join(chunks) + ")\n" + source
    entry = SERVICE + "/maintenance/" + label + ".py"
    shell = ("#!/bin/ksh\numask 077\n"
             "[ -d " + SERVICE + " ] && [ ! -L " + SERVICE + " ] || exit 78\n"
             "[ ! -L " + SERVICE + "/maintenance ] || exit 78\n"
             "mkdir -p " + SERVICE + "/maintenance || exit 78\n"
             "count=$(ls -1 " + SERVICE + "/maintenance | wc -l)\n"
             "used=$(du -sk " + SERVICE + "/maintenance | awk '{print $1}')\n"
             "[ $count -lt 64 ] && [ $used -lt 65536 ] || exit 78\n"
             "test ! -e " + entry + " || exit 78\n"
             "cat > " + entry + " <<'BBATTERY_REVIEWED_SOURCE'\n" + source +
             "\nBBATTERY_REVIEWED_SOURCE\n[ $? -eq 0 ] || exit 78\n"
             "export PYTHONHOME=/usr:/usr\nexport PYTHONDONTWRITEBYTECODE=1\n"
             "/proc/boot/pathtrust '!" + entry + "' || exit 78\n"
             "/base/usr/bin/python3.2 -B " + entry + "\n")
    script = work / (label + ".ksh")
    script.write_text(shell, encoding="utf-8", newline="\n")
    arguments = ["pwsh", "-NoProfile", "-File", str(COORDINATOR), "-Label", label,
                 "-RemoteCommand", "@script:" + script.as_posix()]
    stdout_file = work / (label + '-coordinator.stdout')
    stderr_file = work / (label + '-coordinator.stderr')
    with stdout_file.open('wb') as output, stderr_file.open('wb') as errors:
        process = subprocess.Popen(arguments, stdout=output, stderr=errors)
        try:
            returncode = process.wait(timeout=85)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise RuntimeError('Coordinator observation timed out; preserve and resume intent ' + plan['tag'])
    evidence = EVIDENCE / (label + ".json")
    if not evidence.exists():
        raise RuntimeError(stderr_file.read_text(encoding='utf-8', errors='replace')[-3000:] or "Device lease rejected the operation")
    record = json.loads(evidence.read_text(encoding="utf-8"))
    (work / (label + ".json")).write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
    replies = [line[len("BBATTERY_RELEASE "):] for line in record.get("stdout", "").splitlines()
               if line.startswith("BBATTERY_RELEASE ")]
    if record.get("returncode") != 0 or not replies:
        raise RuntimeError("Operation remains recorded at " + str(evidence) + ": " + record.get("stderr", "")[-2000:])
    return json.loads(replies[-1])


def stage_payloads(plan):
    """BAR bytes use scp; the frozen coordinator executes only the small adapter.

    Q10's legacy SSH/ksh stdin stalls on large heredocs. Transfers share the
    manager lease and never submit installer transactions or execute uploads.
    """
    import device
    if not re.fullmatch(r'bbattery-[A-Za-z0-9-]+', plan['tag']):
        raise ValueError('Unexpected release tag')
    folder = ROOT / 'build/deployments' / plan['tag']
    with device.lease():
        metadata = device.ssh('test -d ' + SERVICE + ' && test ! -L ' + SERVICE +
                              ' && test -d ' + SERVICE + '/maintenance && test ! -L ' + SERVICE +
                              '/maintenance && ls -nd ' + SERVICE + ' ' + SERVICE + '/maintenance')
        rows = [line.split() for line in metadata.splitlines()]
        if len(rows) != 2 or any(len(row) < 4 or row[0] != 'drwx------' or row[2:4] != ['0', '0'] for row in rows):
            raise RuntimeError('Private maintenance staging ownership is unknown')
        for field in ('old', 'new'):
            body = base64.b64decode(plan[field]['base64'])
            if digest(body) != plan[field]['sha256']:
                raise RuntimeError('Saved payload differs')
            local = folder / (field + '.bar')
            local.write_bytes(body)
            remote = SERVICE + '/maintenance/' + plan['tag'] + '-' + field + '.bar'
            exists = device.ssh('test -f ' + remote + ' && print present', check=False).strip()
            readback = folder / (field + '-readback.bar')
            if exists != 'present':
                temporary = remote + '.part-' + uuid.uuid4().hex[:12]
                device.scp(local, temporary)
                device.scp(readback, temporary, True)
                if readback.read_bytes() != body:
                    raise RuntimeError('Payload transfer readback differs')
                device.ssh('test ! -e ' + remote + ' && mv ' + temporary + ' ' + remote)
            device.scp(readback, remote, True)
            if readback.read_bytes() != body:
                raise RuntimeError('Original staged payload differs; preserve it')


def observe(asset_names=None):
    return run("observe", {"tag": "bbattery-observe-" + uuid.uuid4().hex[:16],
                           "assetNames": asset_names or list(package(ROOT / "build/BBattery.bar")["assets"])})


def install():
    current = package(ROOT / "build/BBattery.bar")
    pointer = ROOT / "build/deployments" / (current["sha256"] + ".json")
    if pointer.exists():
        plan = json.loads(pointer.read_text(encoding="utf-8"))
    else:
        old = package(ROOT / "build/releases/0.1.0.8-9ef1927f3308/BBattery.bar")
        before = observe(list(old["assets"]))
        if not before["ready"] or before["unfinished"]:
            raise RuntimeError("Frozen native installer is unavailable or has unresolved transactions")
        if old["version"] != before["version"] or old["assets"] != before["assets"]:
            raise RuntimeError("Installed baseline differs from the preserved 0.1.0.8 BAR")
        plan = {"tag": "bbattery-0109-" + uuid.uuid4().hex[:20], "old": old, "new": current,
                "baseline": before, "starter": starter(before["uid"], before["gid"], before["dname"])}
        plan["intentSha256"] = digest(json.dumps(plan, sort_keys=True, ensure_ascii=True).encode("utf-8"))
        pointer.parent.mkdir(parents=True, exist_ok=True)
        # Saving the stable intent must precede the remote submission.
        with pointer.open("x", encoding="utf-8") as stream:
            json.dump(plan, stream, ensure_ascii=True, sort_keys=True)
            stream.flush()
            import os
            os.fsync(stream.fileno())
    if plan["new"]["sha256"] != current["sha256"]:
        raise RuntimeError("Deployment intent differs; no replacement submission")
    for attempt in range(4):
        result = run("install", plan)
        print("BBattery release: " + result["phase"], flush=True)
        if result["phase"] == "committed":
            receipt = dict(dname=result["dname"], version=current["version"], sha256=current["sha256"],
                           deployment=plan["tag"], verifiedAtUtc=datetime.datetime.fromtimestamp(result["time"], datetime.timezone.utc).isoformat())
            (ROOT / "build/install-evidence.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
            evidence = dict(binary="/apps/" + result["dname"] + "/native/batteryd",
                            data="/accounts/1000/appdata/" + result["dname"] + "/data/battery",
                            pid=result["state"]["collector"]["pid"], uid=plan["baseline"]["uid"],
                            gid=plan["baseline"]["gid"], serviceDirectory=SERVICE, deployment=plan["tag"],
                            bootHook="/apps/sys.android.gYABgKAOw1czN6neiAT72SGO.ns/native/system/xbin/btool",
                            bootVerifiedAfterReboot=False)
            (ROOT / "build/collector-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
            return result
        if result["phase"] not in ("pending", "activation_pending"):
            raise RuntimeError("Release requires review: " + json.dumps(result))
        time.sleep(2)
    raise RuntimeError("Deployment is awaiting confirmation; rerun the same BAR to observe " + plan["tag"])


def starter(uid, gid, dname):
    if not isinstance(uid, int) or not isinstance(gid, int) or uid <= 0 or gid <= 0:
        raise ValueError("Verified non-root application identity required")
    if not dname.startswith("top.blaccat.BBattery.") or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-" for c in dname):
        raise ValueError("Unexpected application identity")
    command = "on -u {}:{},1000 /apps/{}/native/batteryd --data /accounts/1000/appdata/{}/data/battery".format(uid, gid, dname, dname)
    return ("#!/bin/ksh\n# BBATTERY BOUNDED START 0.1.0.9\n"
            "umask 077\nfor attempt in 1 2 3; do\n"
            "    " + command + " --status >/dev/null 2>&1\n    health=$?\n"
            "    [ $health -eq 0 ] && exit 0\n"
            "    [ $health -eq 10 ] || exit 78\n"
            "    on -d -u {}:{},1000 /apps/{}/native/batteryd --data /accounts/1000/appdata/{}/data/battery </dev/null >/dev/null 2>&1\n".format(uid, gid, dname, dname) +
            "    for check in 1 2 3 4 5 6 7 8 9 10; do\n"
            "        sleep 1\n        " + command + " --status >/dev/null 2>&1\n"
            "        health=$?\n        [ $health -eq 0 ] && exit 0\n"
            "        [ $health -eq 10 ] && break\n        [ $health -eq 11 ] || exit 78\n"
            "    done\n    [ $health -eq 10 ] || exit 78\ndone\nexit 78\n")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("observe", "install", "recover"))
    parser.add_argument("--intent", type=pathlib.Path)
    args = parser.parse_args()
    if args.action == "observe":
        reply = observe()
    elif args.action == "install":
        reply = install()
    else:
        if not args.intent:
            parser.error("recover requires the original --intent file")
        reply = run("recover", json.loads(args.intent.read_text(encoding="utf-8")))
    print(json.dumps(reply, ensure_ascii=False, indent=2))
