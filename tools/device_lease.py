"""Retired entry. BBmanager alone owns device execution locks."""
def lease(*args, **kwargs):
    raise RuntimeError("Use tools/device.py submit/read protocol operations; no private device lease")
def coordinated(*args, **kwargs):
    raise RuntimeError("Device coordination belongs to BBmanager's shared queue")
