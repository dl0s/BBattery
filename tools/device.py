"""BBattery-only deployment over the existing strictly pinned Q10 connection."""
import argparse
import datetime
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
PROFILE = pathlib.Path(os.environ["LOCALAPPDATA"]) / "Q10Manager/connection.json"
cfg = json.loads(PROFILE.read_text(encoding="utf-8-sig"))
if cfg["SshUser"] != "root" or not re.fullmatch(r"[A-Za-z0-9.:-]+", cfg["DeviceHost"]):
    raise ValueError("Invalid configured SSH target")
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


def ssh(command, check=True, timeout=90):
    result = subprocess.run([SSH, "-n", "-T", *options, "-p", str(cfg["SshPort"]), target, command],
                            capture_output=True, timeout=timeout)
    if check and result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace") + result.stdout.decode("utf-8", "replace"))
    return result.stdout.decode("utf-8", "replace")


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
    require_root()
    bar = BUILD / "BBattery.bar"
    m = manifest(bar)
    if m.get("Package-Name") != "top.blaccat.BBattery":
        raise RuntimeError("Unexpected package")
    if (BUILD / "install-evidence.json").exists():
        stop_gui()
    digest = hashlib.sha256(bar.read_bytes()).hexdigest()
    remote_name = "BBattery-" + digest[:12] + "-" + uuid.uuid4().hex[:8] + ".bar"
    remote = "/tmp/q10deploy/" + remote_name
    ssh("mkdir -p /tmp/q10deploy")
    scp(bar, remote)
    back = BUILD / "bar.readback"
    scp(back, remote, True)
    if hashlib.sha256(back.read_bytes()).hexdigest() != digest:
        raise RuntimeError("BAR round-trip checksum mismatch")
    ssh(". /base/scripts/sudtools.sh\nsud_install_package_2 " + quote(remote), timeout=150)
    job = ssh("cat " + quote("/pps/system/installer/upd/current/job." + remote_name))
    fields = dict(line.split("::", 1) for line in job.splitlines() if "::" in line)
    if (fields.get("result") != "success" or fields.get("progress") != "100" or
            fields.get("actual_app_version") != m.get("Package-Version") or
            fields.get("actual_name") != m["Package-Name"]):
        raise RuntimeError("Expected package/version not confirmed by installer")
    record = dict(dname=fields["actual_dname"], version=m["Package-Version"], sha256=digest, pps=job,
                  verifiedAtUtc=datetime.datetime.now(datetime.timezone.utc).isoformat())
    (BUILD / "install-evidence.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    _, app, _ = receipt()
    with zipfile.ZipFile(bar) as archive:
        for asset in ("native/bbattery", "native/assets/main.qml", "native/assets/Metric.qml", "native/assets/Chart.qml"):
            copy = BUILD / ("installed-" + asset.rsplit("/", 1)[1])
            scp(copy, app + "/" + asset, True)
            if copy.read_bytes() != archive.read(asset):
                raise RuntimeError("Installed asset readback mismatch: " + asset)
    print("Installed and verified BBattery " + record["version"], flush=True)


def launch():
    r, _, _ = receipt()
    if gui_pids():
        print("BBattery GUI already running; duplicate launch skipped", flush=True)
        return
    message = "msg::start\ndat::" + r["dname"] + ",ORIENTATION=0\nid::bbattery-" + uuid.uuid4().hex
    ssh("print " + quote(message) + " > /pps/services/launcher/control")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        pids = gui_pids()
        if len(pids) == 1:
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


def upload_verified(local, remote, mode="600"):
    local = pathlib.Path(local)
    digest = hashlib.sha256(local.read_bytes()).hexdigest()
    scp(local, remote + ".next")
    staged = remote + ".next"
    ssh("chown 0:0 " + quote(staged) + "\nchmod " + mode + " " + quote(staged) +
        "\nmv " + quote(staged) + " " + quote(remote))
    copy = BUILD / ("readback-" + pathlib.PurePosixPath(remote).name)
    scp(copy, remote, True)
    if hashlib.sha256(copy.read_bytes()).hexdigest() != digest:
        raise RuntimeError("Provisioned file differs: " + remote)
    return digest


def stage_collector(binary):
    binary = pathlib.Path(binary)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    remote = "/var/bbattery/bin/batteryd-" + digest + "-" + uuid.uuid4().hex[:8]
    source = remote + ".source"
    upload_verified(binary, source, "700")
    # Copy, per-file trust and first execution share the same QNX shell context.
    tests = ssh("test ! -e " + quote(remote) + " && cp " + quote(source) + " " + quote(remote) +
                " && /proc/boot/pathtrust " + quote("!" + remote) + " && " + quote(remote) + " --self-test")
    registration = ssh("/proc/boot/pathtrust -t " + quote(remote))
    if ": trusted" not in registration or "RESULT: PASS" not in tests or "PASS: SQLite" not in tests:
        raise RuntimeError("Native collector trust/measurement/storage checks failed")
    copy = BUILD / ("readback-" + pathlib.PurePosixPath(remote).name)
    scp(copy, remote, True)
    if hashlib.sha256(copy.read_bytes()).hexdigest() != digest:
        raise RuntimeError("Final immutable collector differs from the verified source")
    ssh("rm " + quote(source))
    return remote, digest, registration, tests


def provision():
    require_root()
    uid, gid = gui_owner()
    _, _, sandbox = receipt()
    directory = sandbox + "/data/battery"
    ssh("mkdir -p /var/bbattery/bin\nchown 0:0 /var/bbattery /var/bbattery/bin\nchmod 700 /var/bbattery /var/bbattery/bin")
    binary = BUILD / "batteryd"
    remote, digest, registration, tests = stage_collector(binary)
    (BUILD / "native-tests.txt").write_text(tests, encoding="utf-8")
    code = """#!/bin/ksh
# BBATTERY FIXED SUPERVISOR
umask 077
STATE=/var/bbattery
remove_lock() {
    [ -d "$STATE/supervisor.lock" ] && [ ! -L "$STATE/supervisor.lock" ] || return 20
    for entry in "$STATE/supervisor.lock"/* "$STATE/supervisor.lock"/.[!.]* "$STATE/supervisor.lock"/..?*; do
        [ ! -e "$entry" ] && [ ! -L "$entry" ] || return 20
    done
    rm -r "$STATE/supervisor.lock"
}
old=$(cat "$STATE/supervisor.pid" 2>/dev/null)
if ! mkdir "$STATE/supervisor.lock" 2>/dev/null; then
    case "$old" in ''|*[!0-9]*) ;; *) if pidin -p "$old" ar | grep -q '[b]battery-supervisor'; then exit 0; fi ;; esac
    remove_lock || exit 20
    mkdir "$STATE/supervisor.lock" || exit 20
fi
trap 'remove_lock' EXIT
trap 'exit 0' TERM INT
print -r -- "$$" > "$STATE/supervisor.pid"
/proc/boot/pathtrust __TRUST__ || exit 21
while :; do
    __BINARY__ --data __DATA__ --uid __UID__ --gid __GID__ >> "$STATE/service.log" 2>&1
    sleep 2
done
""".replace("__TRUST__", quote("!" + remote)).replace("__BINARY__", quote(remote)).replace(
        "__DATA__", quote(directory)).replace("__UID__", uid).replace("__GID__", gid)
    script = BUILD / "collector-supervisor.sh"
    script.write_text(code, encoding="utf-8", newline="\n")
    old_script = ssh("cat /var/bbattery/start.sh", check=False)
    if old_script:
        if "# BBATTERY FIXED SUPERVISOR" not in old_script or directory not in old_script:
            raise RuntimeError("Unrecognized BBattery supervisor; left unchanged")
        old_supervisor = ssh("cat /var/bbattery/supervisor.pid", check=False).strip()
        old_service = ssh("cat " + quote(directory + "/collector.pid"), check=False).strip()
        if old_supervisor.isdigit() and "bbattery-supervisor" in ssh("pidin -p " + old_supervisor + " ar", check=False):
            ssh("kill -TERM " + old_supervisor)
            if old_service.isdigit() and "/var/bbattery/bin/batteryd-" in ssh("pidin -p " + old_service + " ar", check=False):
                ssh("kill -TERM " + old_service)
            for _ in range(15):
                if "bbattery-supervisor" not in ssh("pidin -p " + old_supervisor + " ar", check=False):
                    break
                time.sleep(1)
            else:
                raise RuntimeError("Old supervisor did not stop; no duplicate launched")
    upload_verified(script, "/var/bbattery/start.sh")
    ssh("on -d /bin/ksh -c " + quote(code) + " bbattery-supervisor </dev/null >>/var/bbattery/boot.log 2>&1")
    deadline = time.monotonic() + 25
    pid = ""
    while time.monotonic() < deadline:
        pid = ssh("cat " + quote(directory + "/collector.pid"), check=False).strip()
        if pid.isdigit():
            identity = ssh("pidin -p " + pid + " users", check=False)
            if re.search(r"\s" + uid + r"\s+" + gid + r"\s+" + uid + r"\s+" + gid + r"\s", identity):
                break
        time.sleep(1)
    else:
        raise RuntimeError("Independent least-privilege collector failed to start: " +
                           ssh("tail -n 20 /var/bbattery/service.log", check=False) +
                           ssh("tail -n 20 /var/bbattery/boot.log", check=False))
    evidence = dict(binary=remote, sha256=digest, data=directory, pid=int(pid), uid=int(uid), gid=int(gid),
                    identity=identity, pathTrust=registration, samplesInterval=10,
                    rootPolicyChanged=False, existingFileServiceChanged=False, bootVerifiedAfterReboot=False)
    (BUILD / "collector-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("Independent collector started; effective UID " + uid, flush=True)


def install_boot_hook():
    """Add only BBattery's bounded startup stanza, preserving every existing stanza."""
    require_root()
    receipt()
    upstream = "/apps/sys.android.gYABgKAOw1czN6neiAT72SGO.ns/native/system/xbin/btool"
    before = BUILD / "boot-source.before"
    scp(before, upstream, True)
    original = before.read_text(encoding="utf-8")
    if not original.startswith("#!/bin/sh\n") or "/usr/sbin/sshd" not in original:
        raise RuntimeError("Unrecognized existing boot source; left unchanged")
    begin = "# BBATTERY COLLECTOR BOOT BEGIN"
    end = "# BBATTERY COLLECTOR BOOT END"
    block = (begin + "\non -d /bin/ksh -c \"$(cat /var/bbattery/start.sh)\" bbattery-supervisor"
             " </dev/null >>/var/bbattery/boot.log 2>&1\n" + end + "\n")
    if begin in original:
        expression = re.compile(r"^" + re.escape(begin) + r"\n.*?^" + re.escape(end) + r"\n?", re.M | re.S)
        if len(expression.findall(original)) != 1:
            raise RuntimeError("Ambiguous application boot hook")
        updated = expression.sub(block, original)
    else:
        updated = original.rstrip("\n") + "\n" + block
    if updated != original:
        patched = BUILD / "boot-source.patched"
        patched.write_text(updated, encoding="utf-8", newline="\n")
        scp(patched, "/var/bbattery/boot-source.next")
        recheck = BUILD / "boot-source.recheck"
        scp(recheck, upstream, True)
        if recheck.read_bytes() != before.read_bytes():
            raise RuntimeError("Boot source changed concurrently; nothing written")
        ssh("test -f /var/bbattery/boot-source.before || cp -p " + quote(upstream) +
            " /var/bbattery/boot-source.before\nchmod 600 /var/bbattery/boot-source.before\n"
            "cat /var/bbattery/boot-source.next > " + quote(upstream))
        scp(BUILD / "boot-source.readback", upstream, True)
        if (BUILD / "boot-source.readback").read_bytes() != patched.read_bytes():
            raise RuntimeError("Boot hook readback failed")
    evidence = json.loads((BUILD / "collector-evidence.json").read_text(encoding="utf-8"))
    evidence.update(bootHookInstalled=True, bootHook=upstream, bootSourceSha256=hashlib.sha256(updated.encode()).hexdigest())
    (BUILD / "collector-evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print("Added BBattery-only boot stanza; actual reboot has NOT been tested", flush=True)


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


def stop_gui():
    pids = gui_pids()
    for pid in pids:
        ssh("kill " + pid)
    deadline = time.monotonic() + 15
    while gui_pids():
        if time.monotonic() >= deadline:
            raise RuntimeError("BBattery GUI did not exit; replacement launch was not attempted")
        time.sleep(0.5)
    print("Verified BBattery GUI stopped (" + str(len(pids)) + " instances); collector unchanged", flush=True)


def app_call(operation, **args):
    _, _, sandbox = receipt()
    uid, gid = gui_owner()
    request_id = uuid.uuid4().hex
    path = sandbox + "/data/diagnostics"
    request = BUILD / "gui-request.json"
    request.write_text(json.dumps(dict(op=operation, requestId=request_id, **args)), encoding="utf-8")
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
                if not response.get("ok"):
                    raise RuntimeError("GUI operation failed: " + str(response))
                return response
        except json.JSONDecodeError:
            pass
        time.sleep(0.4)
    raise RuntimeError("GUI request timed out: " + operation)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("install", "launch", "provision", "boot-hook", "stop-gui", "state"))
    command = parser.parse_args().command
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
    else:
        print(json.dumps(app_call("state"), ensure_ascii=False, indent=2))
