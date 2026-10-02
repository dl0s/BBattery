"""Run production migration, profile-capacity and interval-test SQL locally."""
import json
import pathlib
import re
import sqlite3
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
STORE = (ROOT / "src/store.cpp").read_text(encoding="utf-8")
BACKEND = (ROOT / "src/backend.cpp").read_text(encoding="utf-8")


def literals(source):
    return "".join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', source))


SCHEMA = literals(STORE.split("const char *schema=", 1)[1].split("char *err=0", 1)[0])
PROFILE_SQL = subprocess.run([str(ROOT / "build/capacity-test.exe"), "--profile-sql"],
                             capture_output=True, text=True, check=True).stdout
SCOPE = "session_id IN (SELECT session_id FROM session_batteries WHERE battery_key=?)"
RESULT_SQL = re.search(r'rows=db.query\("([^"]+)"', BACKEND).group(1)


class BatteryProfilesTest(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.execute("INSERT INTO battery_profiles VALUES('spare','备用 A')")
        self.add(1, "legacy", 100000)
        self.add(2, "spare", 120000, reason="battery-marker-changed")
        self.add(3, "legacy", 140000, closed=False)
        self.db.commit()

    def tearDown(self):
        self.db.close()

    def add(self, session, key, stamp, closed=True, reason="state-changed", readings=(90, 30)):
        self.db.execute("INSERT INTO sessions(id,start_ms,last_ms,end_ms,mode,battery_id,reason,"
                        "start_soc,end_soc,sample_count,mah,covered_s,elapsed_s,stable_soc) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (session,stamp,stamp+10000,stamp+10000 if closed else None,"discharge",244,reason,
                         readings[0],readings[-1],len(readings),10,10,10,1))
        self.db.execute("INSERT INTO session_batteries VALUES(?,?)", (session,key))
        self.db.executemany("INSERT INTO samples(utc_ms,mono_ms,run_id,session_id,interval_s,ready,"
                            "battery_id,mode,soc,current,voltage,temperature,gap_reason) VALUES(?,?,'test',?,10,1,244,'discharge',?,-3600,4000,25,'')",
                            ((stamp+i*10000,stamp+i*10000,session,soc) for i,soc in enumerate(readings)))

    def selected(self, key, session=0):
        row = self.db.execute(PROFILE_SQL, ("discharge",key,session,session)).fetchone()
        return row["id"] if row else None

    def test_returning_to_same_marker_reuses_completed_capacity(self):
        self.assertEqual(self.selected("legacy"), 1)
        self.assertEqual(self.selected("spare"), 2)
        self.assertIsNone(self.selected("unused"))
        self.assertIsNone(self.selected("legacy", 2))

    def test_same_system_battery_id_does_not_mix_profiles(self):
        rows = self.db.execute("SELECT id FROM samples WHERE " + SCOPE + " ORDER BY id", ("legacy",)).fetchall()
        self.assertEqual([r[0] for r in rows], [1, 2, 5, 6])
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM samples WHERE " + SCOPE, ("spare",)).fetchone()[0], 2)

    def test_named_battery_keeps_identity_when_system_id_changes(self):
        self.db.execute("UPDATE sessions SET battery_id=245 WHERE id=1")
        self.assertEqual(self.selected("legacy"), 1)

    def test_reversed_soc_is_still_rejected_for_named_battery(self):
        self.add(4, "legacy", 160000, readings=(90,40,60,30))
        self.assertEqual(self.selected("legacy"), 1)

    def test_rename_and_repeat_migration_preserve_records_and_assignments(self):
        samples = [tuple(r) for r in self.db.execute("SELECT * FROM samples ORDER BY id")]
        sessions = [tuple(r) for r in self.db.execute("SELECT * FROM sessions ORDER BY id")]
        self.db.execute("UPDATE battery_profiles SET label='原装重命名' WHERE key='legacy'")
        self.db.commit()
        self.db.executescript(SCHEMA)
        self.assertEqual([tuple(r) for r in self.db.execute("SELECT * FROM samples ORDER BY id")], samples)
        self.assertEqual([tuple(r) for r in self.db.execute("SELECT * FROM sessions ORDER BY id")], sessions)
        self.assertEqual(self.db.execute("SELECT battery_key FROM session_batteries WHERE session_id=2").fetchone()[0], "spare")
        self.assertEqual(self.selected("legacy"), 1)
        self.assertEqual(self.db.execute("PRAGMA integrity_check").fetchone()[0], "ok")

    def test_migration_from_original_schema_is_additive(self):
        self.db.execute("DELETE FROM metadata WHERE key='battery_profiles_migrated'")
        self.db.execute("DROP TABLE session_batteries")
        self.db.execute("DROP TABLE battery_profiles")
        self.db.commit()
        samples = [tuple(r) for r in self.db.execute("SELECT * FROM samples ORDER BY id")]
        sessions = [tuple(r) for r in self.db.execute("SELECT * FROM sessions ORDER BY id")]
        self.db.executescript(SCHEMA)
        self.assertEqual([tuple(r) for r in self.db.execute("SELECT * FROM samples ORDER BY id")], samples)
        self.assertEqual([tuple(r) for r in self.db.execute("SELECT * FROM sessions ORDER BY id")], sessions)
        self.assertEqual(self.db.execute("SELECT COUNT(*) FROM session_batteries WHERE battery_key='legacy'").fetchone()[0], 3)
        self.assertEqual(self.db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_result_list_uses_index_with_large_raw_history(self):
        self.db.executemany("INSERT INTO samples(utc_ms,mono_ms,run_id,session_id,ready,current,mode,gap_reason) "
                            "VALUES(?,?,'bulk',3,1,-100,'discharge','')", ((i,i) for i in range(100000)))
        self.db.executemany("INSERT INTO capacity_tests(id,battery_key,start_ms,start_mono,deadline_mono,requested_s,last_ms,last_mono,status,mode) "
                            "VALUES(?,'legacy',?,?,?,60,?,?,'completed','discharge')",
                            ((str(i),i,i,i+60000,i+60000,i+60000) for i in range(10000)))
        steps = [0]
        def progress():
            steps[0] += 1
            return int(steps[0] > 100)
        self.db.set_progress_handler(progress,100)
        rows = self.db.execute(RESULT_SQL, ('legacy',51)).fetchall()
        self.db.set_progress_handler(None,0)
        self.assertEqual(len(rows),51)
        self.assertEqual(rows[0]['id'],'9999')
        self.assertLess(steps[0],30)

    def test_scene_creation_and_refresh_do_not_read_full_history(self):
        constructor = BACKEND.split('Backend::Backend()',1)[1].split('Backend::~Backend',1)[0]
        self.assertNotIn('db.open(',constructor)
        self.assertNotIn('db.query(',constructor)
        self.assertIn('QTimer::singleShot(0,this,SLOT(refresh()))',constructor)
        refresh = BACKEND.split('void Backend::refresh(){',1)[1].split('QVariantMap Backend::formatTest',1)[0]
        self.assertNotIn('db.query(',refresh)
        reader = BACKEND.split('class TestReader:public QThread',1)[1].split('class ExportWorker',1)[0]
        self.assertNotIn('history(',reader)
        self.assertNotIn('COUNT(',reader)
        self.assertNotIn('SUM(',reader)

    def test_interval_schema_does_not_rewrite_old_rows(self):
        before = [tuple(r) for r in self.db.execute('SELECT * FROM samples ORDER BY id')]
        self.db.commit()
        self.db.executescript(SCHEMA)
        self.assertEqual(before,[tuple(r) for r in self.db.execute('SELECT * FROM samples ORDER BY id')])
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM capacity_tests').fetchone()[0],0)
        self.assertEqual(self.db.execute('PRAGMA foreign_key_check').fetchall(),[])


if __name__ == "__main__":
    unittest.main(verbosity=2)
