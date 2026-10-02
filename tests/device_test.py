"""Q10 acceptance entry point for the current interval-test product."""
import sys
from interval_device_test import test
if __name__=='__main__':test('--smoke' in sys.argv)
