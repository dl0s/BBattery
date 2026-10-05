"""One-shot QNX maintenance. PLAN is supplied by the reviewed host builder."""
import base64
import hashlib
import imp
import io
import json
import os
import re
import signal
import sqlite3
import stat
import subprocess
import sys
import time
import threading
import zipfile

SERVICE = '/accounts/1000/bbattery-service'
FROZEN = '/var/lib/sud-app-unsigned/v1'
BOOT = '/apps/sys.android.gYABgKAOw1czN6neiAT72SGO.ns/native/system/xbin/btool'
DNAME = 'top.blaccat.BBattery.testDev_at_BBattery8620bde8'
APP = '/apps/' + DNAME
DATA = '/accounts/1000/appdata/' + DNAME + '/data/battery'
EXPECTED = {'app_entry.py': '12cce5463fadadd3f65fc39ba77d31f966a6325be8d5996e182f0c4c4f2f0ce0',
            'supervisor.py': '21e35e9f760e6740da3b46310a05a4de6a365b63d18afc987f406bce6be282e8',
            'boot.ksh': 'a4900de496211447e85a9e22e13150d514b1ff9caf188b40e7c063c724b74905',
            'release-integrity.json': 'cd44126d1545886f4584cc09dcb11a927a550c87081e0535b3720d114341ad1b'}


def digest(body):
    return hashlib.sha256(body).hexdigest()


def read(path, maximum=8 * 1024 * 1024, single_link=True):
    info = os.lstat(path)
    if not stat.S_ISREG(info.st_mode) or info.st_size > maximum or (single_link and info.st_nlink != 1):
        raise RuntimeError('Unexpected fixed file: ' + path)
    with open(path, 'rb') as stream:
        body = stream.read(maximum + 1)
    if len(body) > maximum:
        raise RuntimeError('File grew beyond read limit: ' + path)
    return body


def write(path, body, mode=0o600, owner=(0, 0), exclusive=False):
    temporary = path if exclusive else path + '.next.' + str(os.getpid())
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
    with os.fdopen(descriptor, 'wb') as stream:
        stream.write(body)
        stream.flush()
        os.fsync(stream.fileno())
    os.chown(temporary, owner[0], owner[1])
    os.chmod(temporary, mode)
    if not exclusive:
        os.rename(temporary, path)
    if read(path, max(8 * 1024 * 1024, len(body))) != body:
        raise RuntimeError('File readback mismatch: ' + path)


def barrier(owned=None):
    actual = {name: digest(read(FROZEN + '/' + name)) for name in EXPECTED}
    if actual != EXPECTED:
        raise RuntimeError('Frozen unsigned 1.0.9 changed; preserve the current state')
    state = json.load(open(service.RUN + '/status.json'))
    pending = service.unfinished()
    if not service.status_ready(state) or any(name != owned for name in pending):
        raise RuntimeError('Installer ownership/readiness or unfinished transaction prevents maintenance')
    return state, pending


def battery_processes():
    output = service.process_output()
    found = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].isdigit() and fields[1] == APP + '/native/batteryd':
            expected = ['--data', DATA]
            if fields[2:4] != expected:
                raise RuntimeError('Unknown collector arguments; no destructive recovery')
            found.append(int(fields[0]))
    if len(found) > 1:
        raise RuntimeError('Multiple collector process identities')
    return found


def process_identity(pid):
    # QNX /proc ownership is not the process's effective sandbox identity.
    process = subprocess.Popen(['pidin', '-p', str(pid), 'users'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    timer = threading.Timer(4, process.kill)
    timer.daemon = True
    timer.start()
    try:
        chunks, size = [], 0
        while True:
            chunk = os.read(process.stdout.fileno(), 1024)
            if not chunk:
                break
            size += len(chunk)
            if size > 4096:
                raise RuntimeError('Process identity exceeds its observation limit')
            chunks.append(chunk)
        process.wait()
        output = b''.join(chunks)
    finally:
        timer.cancel()
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()
    if process.returncode or len(output) > 4096:
        raise RuntimeError('Process identity observation failed')
    rows = [line.split() for line in output.decode('utf-8').splitlines()
            if line.split() and line.split()[0] == str(pid)]
    if len(rows) != 1 or len(rows[0]) < 6 or not all(value.isdigit() for value in rows[0][4:6]):
        raise RuntimeError('Process identity is unknown')
    return tuple(int(value) for value in rows[0][4:6])


def database_state():
    if not os.path.exists(DATA + '/history.sqlite'):
        return {'running': [], 'control': None, 'integrity': 'absent'}
    connection = sqlite3.connect(DATA + '/history.sqlite', timeout=2)
    try:
        connection.execute('PRAGMA query_only=ON')
        running = connection.execute("SELECT id FROM capacity_tests WHERE status='running' LIMIT 2").fetchall()
        control = connection.execute('SELECT request_id,ok,message FROM test_control WHERE id=1').fetchone()
        return {'running': running, 'control': control,
                'integrity': connection.execute('PRAGMA quick_check').fetchone()[0]}
    finally:
        connection.close()


def assets(names):
    result = {}
    for name in names:
        if not re.match(r'^native/[A-Za-z0-9_.\-/]+\Z', name) or '..' in name.split('/'):
            raise RuntimeError('Unexpected asset path')
        result[name] = digest(read(APP + '/' + name, single_link=False))
    return result


def version():
    body = read(APP + '/META-INF/MANIFEST.MF', 131072).decode('utf-8')
    body = body.replace('\r\n ', '').replace('\n ', '')
    fields = dict(line.split(': ', 1) for line in body.splitlines() if ': ' in line)
    if fields.get('Package-Name') != 'top.blaccat.BBattery':
        raise RuntimeError('Unexpected installed application')
    return fields['Package-Version']


def observe():
    state = json.load(open(service.RUN + '/status.json'))
    pending = service.unfinished()
    ready = service.status_ready(state)
    identity = os.stat('/accounts/1000/appdata/' + DNAME + '/data/gui-identity.json')
    if identity.st_uid <= 0 or identity.st_gid <= 0:
        raise RuntimeError('Application sandbox identity unavailable')
    boot = read(BOOT)
    starter = read(SERVICE + '/start.sh')
    return dict(ready=ready, unfinished=pending, unsigned=state, frozenHashes=EXPECTED,
                nativeUid=os.stat('/proc/' + str(state['service_pid'])).st_uid,
                version=version(), dname=DNAME, uid=identity.st_uid, gid=identity.st_gid,
                bootSha256=digest(boot), bootBase64=base64.b64encode(boot).decode('ascii'),
                startSha256=digest(starter), startBase64=base64.b64encode(starter).decode('ascii'),
                bootMetadata=[stat.S_IMODE(os.stat(BOOT).st_mode), os.stat(BOOT).st_uid, os.stat(BOOT).st_gid],
                settingsSha256=digest(read(DATA + '/settings.ini', 65536)),
                assets=assets(PLAN.get('assetNames', ['native/bbattery', 'native/batteryd', 'native/assets/main.qml', 'native/assets/Metric.qml'])),
                collectorPids=battery_processes(), database=database_state())


def idle(allow_pending=False):
    barrier()
    state = database_state()
    if state['running'] or state['integrity'] not in ('ok', 'absent'):
        raise RuntimeError('Active test or unknown database state prevents update/recovery')
    # A persisted, unacknowledged command must be reconciled, not replaced.
    settings = read(DATA + '/settings.ini', 65536).decode('utf-8')
    group = ''
    request = ''
    for line in settings.splitlines():
        if line.startswith('['):
            group = line.strip()
        elif group == '[test]' and line.startswith('request='):
            request = line[len('request='):].strip()
    if not allow_pending and request and (not state['control'] or request != state['control'][0]):
        raise RuntimeError('Unacknowledged battery operation prevents destructive recovery')


def stop_collector(allow_pending=False):
    idle(allow_pending)
    if allow_pending and battery_processes():
        raise RuntimeError('Pending intent with a current executor prevents destructive activation')
    # Legacy monitor is identified by its recorded PID and reviewed source.
    pidfile = SERVICE + '/supervisor.pid'
    if os.path.exists(pidfile):
        raw = read(pidfile, 64).strip()
        if raw.isdigit():
            pid = int(raw)
            output = service.process_output()
            if re.search(r'^\s*' + str(pid) + r'\s+.*bbattery-supervisor', output, re.M):
                if process_identity(pid) != (0, 0):
                    raise RuntimeError('Legacy supervisor owner is unknown')
                os.kill(pid, signal.SIGTERM)
                deadline = time.time() + 5
                while re.search(r'^\s*' + str(pid) + r'\s+.*bbattery-supervisor', service.process_output(), re.M):
                    if time.time() >= deadline:
                        raise RuntimeError('Legacy supervisor stop not confirmed')
                    time.sleep(.2)
    targets = battery_processes()
    for pid in targets:
        if battery_processes() != [pid] or process_identity(pid) != (PLAN['baseline']['uid'], PLAN['baseline']['gid']):
            raise RuntimeError('Collector identity changed before signal')
        os.kill(pid, signal.SIGTERM)
    deadline = time.time() + 8
    while battery_processes() and time.time() < deadline:
        time.sleep(.2)
    if battery_processes():
        raise RuntimeError('Collector stop not confirmed')


def gui_pids():
    output = service.process_output()
    targets = []
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0].isdigit() and fields[1] == DNAME:
            targets.append(int(fields[0]))
    if len(targets) > 1:
        raise RuntimeError('Multiple GUI process identities')
    return targets


def stop_gui():
    for pid in gui_pids():
        if gui_pids() != [pid] or process_identity(pid) != (PLAN['baseline']['uid'], PLAN['baseline']['gid']):
            raise RuntimeError('Unexpected GUI identity')
        os.kill(pid, signal.SIGTERM)
    deadline = time.time() + 8
    while gui_pids():
        if time.time() >= deadline:
            raise RuntimeError('GUI stop not confirmed')
        time.sleep(.2)


def pps(path):
    raw = read(path, 16384, single_link=False).decode('utf-8')
    fields = {}
    for line in raw.splitlines():
        if '::' in line:
            key, value = line.split('::', 1)
            if key in fields:
                raise RuntimeError('Duplicate installer field')
            fields[key] = value
    return fields


def native_install(record, image, rollback=False):
    field = 'rollbackSubmission' if rollback else 'submission'
    job = 'job.' + PLAN['tag'] + ('-recover' if rollback else '')
    path = service.JOBS + '/' + job
    barrier(job)
    if field not in record:
        if os.path.exists(path):
            raise RuntimeError('Unowned installer object; keep original evidence')
        staging = '/var/tmp/upd/' + PLAN['tag'] + ('-recover' if rollback else '')
        if not os.path.exists(staging):
            os.mkdir(staging, 0o750)
            os.chown(staging, 0, 88)
        body = base64.b64decode(image['base64'].encode('ascii'))
        if digest(body) != image['sha256']:
            raise RuntimeError('Candidate BAR hash differs')
        bar = staging + '/BBattery.bar'
        if not os.path.exists(bar):
            write(bar, body, mode=0o640, owner=(0, 88), exclusive=True)
        if digest(read(bar)) != image['sha256']:
            raise RuntimeError('Original staged BAR differs')
        record[field] = {'job': job, 'status': 'unconfirmed', 'packageSha256': image['sha256']}
        phase('recover_submission' if rollback else 'submission_uncertain')
        gid, mask = os.getegid(), os.umask(0o007)
        try:
            os.setegid(88)
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o660)
        finally:
            os.setegid(gid)
            os.umask(mask)
        try:
            os.fchown(descriptor, 88, 88)
            request = ('action::install\npackage_location::' + bar + '\nextras::\n').encode()
            if os.write(descriptor, request) != len(request):
                raise RuntimeError('Short PPS write; original submission remains unconfirmed')
        finally:
            os.close(descriptor)
        record[field]['status'] = 'submitted'
        phase('recover_pending' if rollback else 'pending')
    if not os.path.exists(path):
        raise RuntimeError('Original submission is unconfirmed; no replacement transaction')
    deadline = time.time() + 25
    while time.time() < deadline:
        result = pps(path)
        record[field]['pps'] = result
        terminal = result.get('result', '')
        if terminal == 'success' or terminal.split(' ', 1)[0] in ('failure', 'failed', 'cancelled', 'error'):
            record[field]['status'] = 'success' if terminal == 'success' else 'failed'
            phase('recover_result' if rollback else 'native_result')
            if terminal != 'success':
                raise RuntimeError('Native installer reported failure: ' + terminal)
            if result.get('actual_dname') != DNAME or result.get('actual_app_version') != image['version']:
                raise RuntimeError('Native terminal identity/version differs')
            if version() != image['version'] or assets(image['assets']) != image['assets']:
                raise RuntimeError('Installed assets do not match the verified BAR')
            return True
        time.sleep(.2)
    phase('recover_pending' if rollback else 'pending')
    return False


def gui_state():
    identity = PLAN['baseline']
    directory = '/accounts/1000/appdata/' + DNAME + '/data/diagnostics'
    request_id = PLAN['tag'] + '-' + str(os.getpid())
    body = json.dumps({'op': 'state', 'requestId': request_id}).encode('utf-8')
    write(directory + '/request.json', body, owner=(identity['uid'], identity['gid']))
    deadline = time.time() + 8
    while time.time() < deadline:
        try:
            reply = json.loads(read(directory + '/response.json', 65536).decode('utf-8'))
            if reply.get('requestId') == request_id:
                return reply
        except (IOError, ValueError):
            pass
        time.sleep(.3)
    return None


def activate(starter, boot, recovering=False):
    barrier()
    source_guard()
    if read(SERVICE + '/start.sh') != starter:
        write(SERVICE + '/start.sh', starter)
    subprocess.check_call(['/proc/boot/pathtrust', '!' + SERVICE + '/start.sh'])
    if read(BOOT) != boot:
        mode, uid, gid = PLAN['baseline']['bootMetadata']
        write(BOOT, boot, mode=mode, owner=(uid, gid))
        subprocess.check_call(['/proc/boot/pathtrust', '!' + BOOT])
    subprocess.check_call(['sync'])
    subprocess.check_call(['on', '-d', '/bin/ksh', SERVICE + '/start.sh'], stdin=open('/dev/null'), stdout=open('/dev/null', 'wb'), stderr=subprocess.STDOUT)
    launcher_field = 'recoveryGuiRequested' if recovering else 'guiRequested'
    confirmed_field = launcher_field + 'Confirmed'
    if not gui_pids():
        if record.get(confirmed_field):
            record[launcher_field] = False
            record[confirmed_field] = False
            phase('activation_pending')
        if not record.get(launcher_field):
            record[launcher_field] = True
            phase('activation_pending')
            descriptor = os.open('/pps/services/launcher/control', os.O_WRONLY)
            try:
                body = ('msg::start\ndat::' + DNAME + ',ORIENTATION=0\nid::' + PLAN['tag']).encode()
                if os.write(descriptor, body) != len(body):
                    raise RuntimeError('Launcher request is unconfirmed')
            finally:
                os.close(descriptor)
    reply = gui_state()
    if not reply or not reply.get('ready') or reply.get('euid') != PLAN['baseline']['uid']:
        phase('activation_pending')
        return False
    collector = reply.get('collector', {})
    if battery_processes() != [collector.get('pid')] or collector.get('error'):
        raise RuntimeError('GUI heartbeat does not belong to the actual collector')
    record['state'] = reply
    record[confirmed_field] = True
    record['unsignedAfter'], pending = barrier()
    if pending or record['unsignedAfter']['service_pid'] != record['unsignedBefore']['service_pid']:
        raise RuntimeError('Frozen native consumer identity changed')
    phase('recovered' if recovering else 'committed')
    return True


def new_boot():
    boot = base64.b64decode(PLAN['baseline']['bootBase64'].encode('ascii'))
    block = (b'# BBATTERY COLLECTOR BOOT BEGIN\nif /proc/boot/pathtrust !/accounts/1000/bbattery-service/start.sh; then\n'
             b'    on -d /bin/ksh /accounts/1000/bbattery-service/start.sh </dev/null >/dev/null 2>&1\nfi\n'
             b'# BBATTERY COLLECTOR BOOT END\n')
    expression = br'^# BBATTERY COLLECTOR BOOT BEGIN\n.*?^# BBATTERY COLLECTOR BOOT END\n?'
    if len(re.findall(expression, boot, re.M | re.S)) != 1:
        raise RuntimeError('Ambiguous BBattery startup stanza')
    return re.sub(expression, block, boot, flags=re.M | re.S)


def source_guard():
    if digest(read(BOOT)) not in (PLAN['baseline']['bootSha256'], digest(new_boot())):
        raise RuntimeError('Boot source changed outside this update')
    if digest(read(SERVICE + '/start.sh')) not in (PLAN['baseline']['startSha256'], digest(PLAN['starter'].encode('utf-8'))):
        raise RuntimeError('Collector source changed outside this update')
    mode, uid, gid = PLAN['baseline']['bootMetadata']
    current = os.stat(BOOT)
    if (stat.S_IMODE(current.st_mode), current.st_uid, current.st_gid) != (mode, uid, gid):
        raise RuntimeError('Boot metadata changed outside this update')


def verify_image(image):
    body = base64.b64decode(image['base64'].encode('ascii'))
    if len(body) > 8 * 1024 * 1024 or digest(body) != image['sha256']:
        raise RuntimeError('Release BAR differs from saved intent')
    with zipfile.ZipFile(io.BytesIO(body)) as archive:
        for name, expected in image['assets'].items():
            if archive.getinfo(name).file_size > 8 * 1024 * 1024 or digest(archive.read(name)) != expected:
                raise RuntimeError('Release asset differs: ' + name)


def backup_data(backup, allow_pending=False):
    # DELETE journaling + a reserved writer lock gives a consistent main file
    # without shutting down the collector before the backup is verified.
    connection = sqlite3.connect(DATA + '/history.sqlite', timeout=2)
    try:
        if connection.execute('PRAGMA journal_mode').fetchone()[0].lower() != 'delete':
            raise RuntimeError('Unknown SQLite journaling mode')
        connection.execute('BEGIN IMMEDIATE')
        idle(allow_pending)
        settings = read(DATA + '/settings.ini', 65536)
        for name in ('history.sqlite', 'settings.ini'):
            maximum = 64 * 1024 * 1024 if name == 'history.sqlite' else 65536
            path = backup + '/' + name
            if not os.path.exists(path):
                write(path, read(DATA + '/' + name, maximum), exclusive=True)
            record['backups'][name] = digest(read(path, maximum))
        if settings != read(DATA + '/settings.ini', 65536) or read(backup + '/settings.ini', 65536) != settings:
            raise RuntimeError('Settings changed during preparation')
        saved = sqlite3.connect(backup + '/history.sqlite')
        try:
            if saved.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise RuntimeError('Database backup integrity failed')
        finally:
            saved.close()
    finally:
        connection.rollback()
        connection.close()


def phase(name):
    record['phase'] = name
    record['time'] = time.time()
    if name in ('committed', 'recovered'):
        record.pop('error', None)
    service.atomic_json(journal, record)


def update():
    global record, journal
    backup = SERVICE + '/updates/' + PLAN['tag']
    journal = backup + '/journal.json'
    intent = dict(PLAN)
    intent.pop('action', None)
    expected_intent = intent.pop('intentSha256', None)
    if digest(json.dumps(intent, sort_keys=True, ensure_ascii=True).encode('utf-8')) != expected_intent:
        raise RuntimeError('Deployment parameters differ from the saved intent')
    verify_image(PLAN['old'])
    verify_image(PLAN['new'])
    source_guard()
    if os.path.exists(journal):
        record = json.load(open(journal))
        if record.get('intentSha256') != expected_intent:
            raise RuntimeError('Previous intent differs; do not replace it')
    else:
        already_installed = version() == PLAN['new']['version'] and assets(PLAN['new']['assets']) == PLAN['new']['assets']
        idle(already_installed)
        baseline = PLAN['baseline']
        if not already_installed and (version() != PLAN['old']['version'] or assets(PLAN['old']['assets']) != PLAN['old']['assets']):
            raise RuntimeError('Old application differs from inspected baseline')
        if digest(read(BOOT)) != baseline['bootSha256'] or digest(read(SERVICE + '/start.sh')) != baseline['startSha256']:
            raise RuntimeError('Startup source changed since inspection')
        updates = SERVICE + '/updates'
        if os.path.exists(updates) and len(os.listdir(updates)) >= 8:
            raise RuntimeError('Update history is full; preserve recovery records and refuse new updates')
        os.makedirs(backup, 0o700)
        record = dict(newSha256=PLAN['new']['sha256'], oldSha256=PLAN['old']['sha256'], dname=DNAME,
                      intentSha256=expected_intent, unsignedBefore=barrier()[0], backups={},
                      alreadyInstalled=already_installed)
        phase('preparing')
    if record['phase'] == 'committed' and PLAN['action'] != 'recover':
        barrier()
        if version() != PLAN['new']['version'] or assets(PLAN['new']['assets']) != PLAN['new']['assets']:
            raise RuntimeError('Previously committed application differs')
        # Provision also reconciles the current process, rather than returning
        # the PID recorded by a previous activation.
        activate(PLAN['starter'].encode('utf-8'), new_boot())
        return record
    if PLAN['action'] == 'recover':
        if record['phase'] == 'recovered':
            barrier()
            return record
        if os.path.exists(service.JOBS + '/job.' + PLAN['tag']):
            if not pps(service.JOBS + '/job.' + PLAN['tag']).get('result'):
                raise RuntimeError('Original installer transaction is unresolved; recovery denied')
        for name, expected in record['backups'].items():
            if digest(read(backup + '/' + name, 64 * 1024 * 1024)) != expected:
                raise RuntimeError('Recovery backup differs')
        if set(record['backups']) != set(('baseline.bar', 'start.sh', 'btool', 'history.sqlite', 'settings.ini')):
            raise RuntimeError('Incomplete recovery backup')
        if version() not in (PLAN['old']['version'], PLAN['new']['version']):
            raise RuntimeError('Unknown installed version prevents recovery')
        image = PLAN['old'] if version() == PLAN['old']['version'] else PLAN['new']
        if assets(image['assets']) != image['assets']:
            raise RuntimeError('Unknown installed asset prevents recovery')
        if 'rollbackSubmission' not in record:
            idle()
            stop_gui()
            stop_collector()
        if native_install(record, PLAN['old'], rollback=True):
            activate(read(backup + '/start.sh'), read(backup + '/btool'), recovering=True)
        return record
    if record['phase'] in ('preparing', 'prepared', 'stopping'):
        if record['phase'] == 'preparing':
            items = {'baseline.bar': base64.b64decode(PLAN['old']['base64'].encode('ascii')),
                     'start.sh': read(SERVICE + '/start.sh'), 'btool': read(BOOT)}
            for name, body in items.items():
                path = backup + '/' + name
                if not os.path.exists(path):
                    write(path, body, exclusive=True)
                if read(path) != body:
                    raise RuntimeError('Prepared backup changed')
                record['backups'][name] = digest(body)
            backup_data(backup, record.get('alreadyInstalled', False))
            subprocess.check_call(['sync'])
            phase('prepared')
        source_guard()
        idle(record.get('alreadyInstalled', False))
        if digest(read(DATA + '/settings.ini', 65536)) != record['backups']['settings.ini']:
            raise RuntimeError('User settings changed after the verified backup')
        phase('stopping')
        stop_gui()
        stop_collector(record.get('alreadyInstalled', False))
        phase('stopped')
    if record['phase'] == 'recovered':
        raise RuntimeError('Recovered intent cannot be replayed; inspect the original record')
    source_guard()
    if record.get('alreadyInstalled') or native_install(record, PLAN['new']):
        if version() != PLAN['new']['version'] or assets(PLAN['new']['assets']) != PLAN['new']['assets']:
            raise RuntimeError('Installed candidate changed before activation')
        phase('service_committing')
        activate(PLAN['starter'].encode('utf-8'), new_boot())
    return record


if __name__ == '__main__':
    if os.geteuid() != 0 or not re.match(r'^bbattery-[A-Za-z0-9\-]+\Z', PLAN.get('tag', '')):
        raise RuntimeError('Unexpected maintenance identity')
    for name, expected in EXPECTED.items():
        if digest(read(FROZEN + '/' + name)) != expected:
            raise RuntimeError('Frozen unsigned baseline mismatch')
    service = imp.load_source('bbattery_frozen_service', FROZEN + '/supervisor.py')
    try:
        if PLAN['action'] != 'observe':
            for field in ('old', 'new'):
                if 'base64' not in PLAN[field]:
                    body = read(SERVICE + '/maintenance/' + PLAN['tag'] + '-' + field + '.bar')
                    PLAN[field]['base64'] = base64.b64encode(body).decode('ascii')
        result = observe() if PLAN['action'] == 'observe' else update()
        # Payload and preserved backups stay on device; responses remain bounded.
        print('BBATTERY_RELEASE ' + json.dumps(result, sort_keys=True))
    except Exception as error:
        if 'record' in globals() and 'journal' in globals():
            record['error'] = str(error)[:2048]
            service.atomic_json(journal, record)
        raise
