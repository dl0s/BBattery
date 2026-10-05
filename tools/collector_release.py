"""Current BAR deployment adapter; collector activation is a separate capability."""
from device import install
def run(*args, **kwargs):
    raise RuntimeError("Legacy collector maintenance retired; use original deployment ID queries")
