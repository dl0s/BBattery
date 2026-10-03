"""BBattery-only deployment over the existing strictly pinned Q10 connection."""
import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import time
import uuid
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
SERVICE = "/accounts/1000/bbattery-service"
from device_lease import lease, coordinated
def connection_profile(local_appdata):
    root = pathlib.Path(local_appdata)
    for relative in ("Q10Manager/connection.json", "Q10Deploy/config.json"):
        profile = root / relative
        if profile.is_file():
            return profile
    raise FileNotFoundError("No existing Q10Manager or Q10Deploy connection profile")


PROFILE = connection_profile(os.environ["LOCALAPPDATA"])
cfg = json.loads(PROFILE.read_text(encoding="utf-8-sig"))
if cfg["SshUser"] != "root" or not re.fullmatch(r"[A-Za-z0-9.:-]+", cfg["DeviceHost"]):
    raise ValueError("Invalid configured SSH target")
if cfg["DeviceHost"] != "192.168.1.61" or int(cfg["SshPort"]) != 22:
    raise ValueError("BBattery maintenance requires the verified Q10 target")
for name in ("SshKeyPath", "KnownHostsPath"):
    if not pathlib.Path(cfg[name]).is_file():
        raise ValueError("Missing existing connection file: " + name)
SSH = str(pathlib.Path(os.environ["WINDIR"]) / "System32/OpenSSH/ssh.exe")
SCP = str(pathlib.Path(os.environ["WINDIR"]) / "System32/OpenSSH/scp.exe")
options = ["-F", "none", "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
           "-o", "StrictHostKeyChecking=yes", "-o", "GlobalKnownHostsFile=none",
           "-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedKeyTypes=+ssh-rsa",
           "-o", "MACs=+hmac-sha1", "-o", "ConnectTimeout=8",
           "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
           "-o", "UserKnownHostsFile=" + cfg["KnownHostsPath"], "-i", cfg["SshKeyPath"]]
target = cfg["SshUser"] + "@" + cfg["DeviceHost"]
quote = shlex.quote


@coordinated
def ssh(command, check=True, timeout=90):
    result = subprocess.run([SSH, "-n", "-T", *options, "-p", str(cfg["SshPort"]), target, command],
                            capture_output=True, timeout=timeout)
    evidence = BUILD / "device-observations"
    evidence.mkdir(parents=True, exist_ok=True)
    record = dict(command=command, returncode=result.returncode,
                  stdout=result.stdout[:2097152].decode("utf-8", "replace"),
                  stderr=result.stderr[:65536].decode("utf-8", "replace"))
    (evidence / (uuid.uuid4().hex + ".json")).write_text(json.dumps(record, ensure_ascii=False), encoding="utf-8")
    if len(result.stdout) > 2097152 or len(result.stderr) > 65536:
        raise RuntimeError("Device observation exceeds its bounded response size")
    if check and result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace") + result.stdout.decode("utf-8", "replace"))
    return result.stdout.decode("utf-8", "replace")


@coordinated
def scp(local, remote, download=False):
    arguments = [target + ":" + remote, str(local)] if download else [str(local), target + ":" + remote]
    result = subprocess.run([SCP, "-O", *options, "-P", str(cfg["SshPort"]), *arguments],
                            capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))


def require_root():
    output = ssh("pidin -p $$ users")
    rows = [line.split() for line in output.splitlines() if re.match(r"^\s*\d+\s", line)]
    if len(rows) != 1 or rows[0][2] != "0" or rows[0][4] != "0":
        raise RuntimeError("Actual SSH UID and effective UID 0 were not verified")


def manifest(bar):
    with zipfile.ZipFile(bar) as archive:
        text = re.sub(r"\r?\n ", "", archive.read("META-INF/MANIFEST.MF").decode("utf-8"))
    return dict(line.split(": ", 1) for line in text.splitlines() if ": " in line)


def receipt():
    r = json.loads((BUILD / "install-evidence.json").read_text(encoding="utf-8"))
    name = r["dname"]
    if not re.fullmatch(r"top\.blaccat\.BBattery\.[A-Za-z0-9_.-]+", name):
        raise RuntimeError("Unexpected application identity")
    return r, "/apps/" + name, "/accounts/1000/appdata/" + name


def install():
    import collector_release
    return collector_release.install()


@coordinated
def launch():
    r, _, _ = receipt()
    if gui_pids():
        print("BBattery GUI already running; duplicate launch skipped", flush=True)
        return
    pending = BUILD / ("gui-launch-" + r.get("deployment", r["version"]) + ".json")
    intent = json.loads(pending.read_text(encoding="utf-8")) if pending.exists() else {}
    if intent.get("status") != "unconfirmed":
        intent = dict(status="unconfirmed", id="bbattery-" + uuid.uuid4().hex, dname=r["dname"])
        with pending.open("w", encoding="utf-8") as stream:
            json.dump(intent, stream)
            stream.flush()
            os.fsync(stream.fileno())
        message = "msg::start\ndat::" + r["dname"] + ",ORIENTATION=0\nid::" + intent["id"]
        ssh("print " + quote(message) + " > /pps/services/launcher/control")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        pids = gui_pids()
        if len(pids) == 1:
            intent["status"] = "confirmed"
            intent["pid"] = pids[0]
            pending.write_text(json.dumps(intent), encoding="utf-8")
            print("Verified normal sandbox GUI launch; PID " + pids[0], flush=True)
            return
        if len(pids) > 1:
            raise RuntimeError("Multiple GUI instances appeared; no further launch requested")
        time.sleep(0.5)
    raise RuntimeError("Normal launcher did not start the GUI within 30 seconds; request may be delayed, not retried")


def gui_owner():
    _, _, data = receipt()
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        text = ssh("ls -nd " + quote(data + "/data/gui-identity.json"), check=False).split()
        if len(text) >= 4 and text[2].isdigit() and text[3].isdigit() and text[2] != "0":
            return text[2], text[3]
        time.sleep(1)
    raise RuntimeError("Normal GUI sandbox identity not established")


@coordinated
def installed_collector(uid, gid):
    """Run the verified package asset with the app identity, including its self-test."""
    _, app, sandbox = receipt()
    remote = app + "/native/batteryd"
    with zipfile.ZipFile(BUILD / "BBattery.bar") as archive:
        expected = archive.read("native/batteryd")
    copy = BUILD / "installed-batteryd"
    scp(copy, remote, True)
    if copy.read_bytes() != expected:
        raise RuntimeError("Installed collector differs from the package; install the current BAR first")
    temporary = sandbox + "/data/diagnostics/collector-self-test"
    ssh("mkdir -p " + quote(temporary) + "\nchown " + uid + ":" + gid + " " + quote(temporary) +
        "\nchmod 700 " + quote(temporary))
    command = "on -e " + quote("TMPDIR=" + temporary) + " -u " + uid + ":" + gid + ",1000 " + quote(remote) + " --self-test"
    tests = ssh(command)
    if "RESULT: PASS" not in tests or "PASS: SQLite" not in tests:
        raise RuntimeError("Packaged collector measurement/storage checks failed")
    return remote, hashlib.sha256(expected).hexdigest(), tests


def provision():
    # Installation and startup use the same persisted release intent and backups.
    return install()


def install_boot_hook():
    return install()


def gui_pids():
    r, _, _ = receipt()
    pids = []
    for line in ssh("pidin ar").splitlines():
        m = re.fullmatch(r"\s*(\d+)\s+" + re.escape(r["dname"]) + r"\s*", line)
        if m:
            expected = r"\s*" + m[1] + r"\s+" + re.escape(r["dname"]) + r"\s*"
            if any(re.fullmatch(expected, row) for row in ssh("pidin -p " + m[1] + " ar").splitlines()):
                pids.append(m[1])
    return pids


@coordinated
def stop_gui():
    pids = gui_pids()
    for pid in pids:
        uid, gid = gui_owner()
        identity = ssh("pidin -p " + pid + " users")
        if not re.search(r"\s" + uid + r"\s+" + gid + r"\s+" + uid + r"\s+" + gid + r"\s", identity):
            raise RuntimeError("GUI identity changed before signal")
        ssh("kill " + pid)
    deadline = time.monotonic() + 15
    while gui_pids():
        if time.monotonic() >= deadline:
            raise RuntimeError("BBattery GUI did not exit; replacement launch was not attempted")
        time.sleep(0.5)
    print("Verified BBattery GUI stopped (" + str(len(pids)) + " instances); collector unchanged", flush=True)


@coordinated
def app_call(operation, **args):
    _, _, sandbox = receipt()
    uid, gid = gui_owner()
    request_id = uuid.uuid4().hex
    path = sandbox + "/data/diagnostics"
    folder = BUILD / "device-observations"
    folder.mkdir(parents=True, exist_ok=True)
    request = folder / (request_id + "-request.json")
    with request.open("w", encoding="utf-8") as stream:
        json.dump(dict(op=operation, requestId=request_id, **args),stream)
        stream.flush()
        os.fsync(stream.fileno())
    scp(request, path + "/request.next")
    ssh("chown " + uid + ":" + gid + " " + quote(path + "/request.next") +
        "\nchmod 600 " + quote(path + "/request.next") +
        "\nmv " + quote(path + "/request.next") + " " + quote(path + "/request.json"))
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        text = ssh("cat " + quote(path + "/response.json"), check=False)
        try:
            response = json.loads(text)
            if response.get("requestId") == request_id:
                request.with_name(request_id + "-response.json").write_text(text, encoding="utf-8")
                if not response.get("ok"):
                    raise RuntimeError("GUI operation failed: " + str(response))
                return response
        except json.JSONDecodeError:
            pass
        time.sleep(0.4)
    raise RuntimeError("GUI request timed out: " + operation)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("install", "launch", "provision", "boot-hook", "stop-gui", "state", "recover"))
    parser.add_argument("--intent", type=pathlib.Path)
    args = parser.parse_args()
    command = args.command
    if command == "install":
        install()
    elif command == "launch":
        launch()
    elif command == "provision":
        provision()
    elif command == "boot-hook":
        install_boot_hook()
    elif command == "stop-gui":
        stop_gui()
    elif command == "recover":
        if not args.intent:
            parser.error("recover requires the original --intent file")
        import collector_release
        print(json.dumps(collector_release.run("recover", json.loads(args.intent.read_text(encoding="utf-8"))), indent=2))
    else:
        print(json.dumps(app_call("state"), ensure_ascii=False, indent=2))
