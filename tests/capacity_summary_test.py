"""Execute production C++ summary SQL against retained and adversarial SQLite data."""
import pathlib
import sqlite3
import subprocess
import unittest
from battery_profiles_test import SCHEMA
ROOT = pathlib.Path(__file__).resolve().parents[1]
SQL = subprocess.run([str(ROOT/'build/capacity-test.exe'),'--summary-sql'],capture_output=True,text=True,check=True).stdout.strip()
INDEX = subprocess.run([str(ROOT/'build/capacity-test.exe'),'--summary-index'],capture_output=True,text=True,check=True).stdout.strip()
class CapacitySummaryTest(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(':memory:'); self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.execute("INSERT INTO battery_profiles VALUES('spare','备用 B')")
    def tearDown(self): self.db.close()
    def add(self,id='good',mode='discharge',key='legacy',stamp=100000,**changes):
        row = dict(id=id,battery_key=key,start_ms=stamp-60000,start_mono=100000,deadline_mono=160000,requested_s=60,last_ms=stamp,last_mono=160000,end_ms=stamp,status='completed',reason='timer',mode=mode,samples=3,
                   discharge_mah=600 if mode=='discharge' else 0,charge_mah=600 if mode=='charge' else 0,
                   elapsed_s=60,integrated_s=60,start_soc=80 if mode=='discharge' else 20,end_soc=50 if mode=='discharge' else 50,stable_soc=1,capacity=2000)
        row.update(changes)
        self.db.execute('INSERT INTO capacity_tests ('+','.join(row)+') VALUES ('+','.join('?' for _ in row)+')',list(row.values()))
    def candidate(self,mode='discharge',key='legacy'):
        return self.db.execute(SQL,(key,mode)).fetchone()
    def chosen(self,key='legacy'):
        discharge=self.candidate('discharge',key); charge=self.candidate('charge',key)
        return discharge if discharge is not None else charge
    def test_discharge_priority_and_separate_charge_estimate(self):
        self.add('d',stamp=100000);self.add('c','charge',stamp=200000)
        self.assertEqual(self.chosen()['id'],'d'); self.assertEqual(self.candidate('charge')['id'],'c')
    def test_charge_fallback_when_no_valid_discharge(self):
        self.add('c','charge');self.add('d',status='interrupted')
        self.assertEqual(self.chosen()['id'],'c')
    def test_short_failed_interrupted_running_and_zero_never_erase_history(self):
        self.add()
        for i,change in enumerate(({'end_soc':51},{'status':'failed'},{'status':'interrupted'},{'status':'running'},{'capacity':None},{'capacity':0})):
            self.add('bad'+str(i),stamp=200000+i,**change)
        self.assertEqual(self.chosen()['id'],'good');self.assertEqual(self.chosen()['end_ms'],100000)
    def test_quality_boundaries_and_nulls(self):
        self.add()
        for field,value in [('integrated_s',56.999),('integrated_s',60.061),('stable_soc',0),('start_soc',None),('end_soc',None),('start_soc',101),('samples',1),('elapsed_s',0),('capacity',float('inf')),('capacity',float('nan')),('capacity',600),('reason','clock-change'),('end_ms',None)]:
            with self.subTest(field=field,value=value):
                self.db.execute("DELETE FROM capacity_tests WHERE id='bad'")
                self.add('bad',stamp=200000,**{field:value})
                self.assertEqual(self.chosen()['id'],'good')
        self.add('boundary',stamp=300000,integrated_s=57)
        self.assertEqual(self.chosen()['id'],'boundary')
    def test_legal_zero_soc_is_not_null(self):
        self.add(start_soc=30,end_soc=0)
        self.assertEqual(self.chosen()['end_soc'],0)
    def test_source_timestamp_orders_estimates_independently_of_page_refresh_and_start(self):
        self.add('first',stamp=300000);self.add('second',stamp=200000,start_ms=190000)
        self.assertEqual(self.chosen()['id'],'first')
    def test_a_b_a_rename_preserves_estimate_and_identity(self):
        self.add('a');self.add('b',key='spare',stamp=200000)
        before=[tuple(row) for row in self.db.execute('SELECT * FROM capacity_tests ORDER BY id')]
        self.assertEqual(self.chosen('legacy')['id'],'a');self.assertEqual(self.chosen('spare')['id'],'b')
        self.db.execute("UPDATE battery_profiles SET label='原装重命名' WHERE key='legacy'")
        self.assertEqual(self.chosen('legacy')['id'],'a')
        self.assertEqual([tuple(row) for row in self.db.execute('SELECT * FROM capacity_tests ORDER BY id')],before)
        self.assertIsNone(self.chosen('unused'))
    def test_old_database_additive_indexes_preserve_rows_and_restore_copy(self):
        self.add();self.db.commit()
        backup=sqlite3.connect(':memory:');self.db.backup(backup)
        original=[tuple(row) for row in self.db.execute('SELECT * FROM capacity_tests')]
        self.db.executescript(SCHEMA)
        self.assertEqual([tuple(row) for row in self.db.execute('SELECT * FROM capacity_tests')],original)
        self.assertEqual(self.chosen()['id'],'good')
        self.assertEqual(backup.execute('SELECT id FROM capacity_tests').fetchone()[0],'good')
        self.assertEqual(self.db.execute('PRAGMA integrity_check').fetchone()[0],'ok');backup.close()
    def test_legacy_monitor_sessions_never_become_timed_test_estimates(self):
        self.db.execute("INSERT INTO sessions(start_ms,last_ms,end_ms,mode,start_soc,end_soc,mah,covered_s,elapsed_s,stable_soc) VALUES(1,61,61,'discharge',80,20,1200,60,60,1)")
        self.assertIsNone(self.chosen())
    def test_list_query_never_reads_raw_samples_and_uses_bb10_compatible_index(self):
        self.add()
        self.assertNotIn('samples ',SQL)
        # Many tests and a large sample table: work is on aggregate tests only.
        for i in range(10000):self.add('bulk'+str(i),stamp=500000+i)
        plan=' '.join(str(tuple(row)) for row in self.db.execute('EXPLAIN QUERY PLAN '+SQL,('legacy','discharge')))
        self.assertIn('capacity_tests_summary_fallback',plan)
        ticks=[0]
        def progress(): ticks[0]+=1;return int(ticks[0]>50)
        self.db.set_progress_handler(progress,100)
        self.assertEqual(self.chosen()['id'],'bulk9999')
        self.db.set_progress_handler(None,0)
        self.assertLess(ticks[0],50)
    def test_optional_partial_index_never_required_for_bb10(self):
        self.add(); self.db.execute(INDEX)
        self.assertEqual(self.chosen()['id'],'good')
if __name__ == '__main__': unittest.main(verbosity=2)
