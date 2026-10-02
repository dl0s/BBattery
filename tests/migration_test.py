"""Read-only Q10 migration acceptance for retained history and battery identities."""
import json,pathlib,sqlite3,sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import device

def test():
    source=ROOT/'build/interval-before.sqlite'
    assert source.exists(),'Pre-upgrade backup required'
    result=device.app_call('backup')
    target=ROOT/'build/migration-after.sqlite'
    device.scp(target,result['path'],True)
    checks=[]
    with sqlite3.connect(source) as old,sqlite3.connect(target) as current:
        assert current.execute('PRAGMA integrity_check').fetchone()[0]=='ok'
        assert not current.execute('PRAGMA foreign_key_check').fetchall()
        for table in ['samples','sessions','snapshots','session_batteries','battery_profiles']:
            rows=old.execute('SELECT * FROM '+table+' ORDER BY 1').fetchall()
            retained=current.execute('SELECT * FROM '+table+' ORDER BY 1').fetchall()
            assert retained[:len(rows)]==rows,table+' changed during migration'
            checks.append({'table':table,'retainedRows':len(rows)})
            print('PASS '+table+': all '+str(len(rows))+' original rows unchanged',flush=True)
    (ROOT/'build/migration-tests.json').write_text(json.dumps({'result':'PASS','checks':checks},indent=2),encoding='utf-8')
if __name__=='__main__':test()
