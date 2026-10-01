"""Exercise the production capacity selection SQL against isolated history."""
from pathlib import Path
import re
import sqlite3
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
SQL = subprocess.run([str(ROOT / "build/capacity-test.exe"), "--sql"], capture_output=True, text=True, check=True).stdout
INDEX = re.search(r'"(CREATE INDEX IF NOT EXISTS samples_session_order[^\"]+)"',
                  (ROOT / "src/store.cpp").read_text(encoding="utf-8")).group(1)

class CapacityHistoryTest(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.executescript("""
            CREATE TABLE sessions(id INTEGER PRIMARY KEY,mode TEXT,battery_id INTEGER,
                end_ms INTEGER,elapsed_s REAL,covered_s REAL,stable_soc INTEGER,mah REAL,
                start_soc INTEGER,end_soc INTEGER,reason TEXT);
            CREATE TABLE samples(id INTEGER PRIMARY KEY,session_id INTEGER,soc REAL);
        """)
        self.db.execute(INDEX)
        self.add(1, "discharge", [90, 60, 30])
        self.add(2, "charge", [20, 50, 80])

    def tearDown(self):
        self.db.close()

    def add(self, id, mode, readings, battery=244, closed=True, coverage=1, reason="state-changed"):
        self.db.execute("INSERT INTO sessions VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            (id,mode,battery,1000 if closed else None,600,600*coverage,1,1200,readings[0],readings[-1],reason))
        self.db.executemany("INSERT INTO samples(session_id,soc) VALUES(?,?)", ((id, soc) for soc in readings))

    def selected(self, mode="discharge", battery=244, session=0):
        row = self.db.execute(SQL, (mode,battery,session,session,session)).fetchone()
        return None if row is None else row[0]

    def test_estimates_use_separate_charge_and_discharge_sessions(self):
        self.assertEqual(self.selected(), 1)
        self.assertEqual(self.selected("charge"), 2)

    def test_live_or_incomplete_newer_session_does_not_replace_valid_result(self):
        self.add(3, "discharge", [90,30], closed=False)
        self.add(4, "discharge", [90,30], coverage=.9)
        self.assertEqual(self.selected(), 1)

    def test_legacy_charge_reversal_is_rejected_even_with_old_stable_flag(self):
        self.add(3, "charge", [20,60,None,40,80])
        self.assertEqual(self.selected("charge"), 2)
        self.assertIsNone(self.selected("charge", session=3))

    def test_soc_reversal_in_discharge_is_rejected(self):
        self.add(3, "discharge", [90,40,None,60,30])
        self.assertEqual(self.selected(), 1)

    def test_small_soc_jitter_does_not_discard_valid_interval(self):
        self.add(3, "charge", [20,40,38,80])
        self.assertEqual(self.selected("charge"), 3)

    def test_old_battery_result_is_not_reused(self):
        self.add(3, "discharge", [90,30], reason="battery-changed")
        self.assertIsNone(self.selected())
        self.add(4, "discharge", [90,30], battery=245)
        self.assertIsNone(self.selected(battery=244))
        self.assertEqual(self.selected(battery=245), 4)

    def test_session_detail_does_not_borrow_another_sessions_estimate(self):
        self.assertIsNone(self.selected("charge", session=1))
        self.assertEqual(self.selected("charge", session=2), 2)

    def test_historical_detail_can_show_its_own_previous_battery(self):
        self.add(3, "discharge", [90,30], reason="battery-changed")
        self.assertIsNone(self.selected("charge"))
        self.assertEqual(self.selected("charge", session=2), 2)

    def test_long_completed_interval_has_bounded_sql_work(self):
        self.add(3, "discharge", [90 - 60 * i / 3999 for i in range(4000)])
        callbacks = [0]
        def progress():
            callbacks[0] += 1
            return int(callbacks[0] > 1000)
        self.db.set_progress_handler(progress, 1000)
        self.assertEqual(self.selected(), 3)
        self.db.set_progress_handler(None, 0)

if __name__ == "__main__":
    unittest.main(verbosity=2)
