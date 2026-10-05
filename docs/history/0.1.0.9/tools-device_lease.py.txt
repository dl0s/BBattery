"""Use Q10Manager's existing Windows share-none device lease for observers.

Privileged releases use the frozen with_device_lock.ps1 entry directly.
Nested GUI/SSH/scp observations share one lease; lock files are never deleted.
"""
import contextlib
import ctypes
import functools
import hashlib
import json
import os
import pathlib
import threading
from ctypes import wintypes

_local = threading.local()
RESOURCE = 'ssh:root@192.168.1.61:22'


@contextlib.contextmanager
def lease():
    if getattr(_local, 'held', False):
        yield
        return
    root = pathlib.Path(os.environ['LOCALAPPDATA']) / 'Q10Manager/Operations/v1'
    locks = root / 'locks'
    locks.mkdir(parents=True, exist_ok=True)
    name = hashlib.sha256(RESOURCE.encode()).hexdigest() + '.lock'
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(locks / name), 0xC0000000, 0, None, 4, 128, None)
    if handle == ctypes.c_void_p(-1).value:
        raise RuntimeError('Q10 shared device lease unavailable: ' + str(ctypes.get_last_error()))
    try:
        jobs = list((root / 'jobs').glob('*/state.json'))
        if len(jobs) > 4096:
            raise RuntimeError('Device queue observation exceeds its limit')
        for state_path in jobs:
            request_path = state_path.with_name('request.json')
            if request_path.exists():
                def read_record(path):
                    with path.open('rb') as stream:
                        body = stream.read(1048577)
                    if len(body) > 1048576:
                        raise RuntimeError('Device queue record exceeds 1 MiB')
                    return json.loads(body.decode('utf-8-sig'))
                state = read_record(state_path)
                request = read_record(request_path)
                if request.get('connection', {}).get('deviceHost') == '192.168.1.61' and state.get('status') in ('queued', 'running', 'unconfirmed'):
                    raise RuntimeError('Device queue requires attention: ' + str(state.get('id')))
        _local.held = True
        yield
    finally:
        _local.held = False
        kernel.CloseHandle(handle)


def coordinated(function):
    @functools.wraps(function)
    def call(*args, **kwargs):
        with lease():
            return function(*args, **kwargs)
    return call
