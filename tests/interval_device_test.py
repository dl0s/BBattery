"""Focused real-Q10 interval acceptance; records actual battery readings only."""
import json,pathlib,sqlite3,sys,time,urllib.parse
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import device
BUILD=ROOT/'build'
SCREENS=BUILD/'screenshots'
SCREENS.mkdir(exist_ok=True)

def state():
    return device.app_call('state')
def wait_for(predicate,timeout=30):
    end=time.monotonic()+timeout
    while True:
        device.app_call('refresh')
        result=state()
        if predicate(result):return result
        assert time.monotonic()<end,'Timed out waiting for collector/UI confirmation'
        time.sleep(1)
def backup(name):
    result=device.app_call('backup')
    target=BUILD/name
    device.scp(target,result['path'],True)
    return target
def capture(name):
    result=device.app_call('capture')
    path=result['path']
    if path.startswith('file:'):path=urllib.parse.urlparse(path).path
    _,_,sandbox=device.receipt()
    assert path.startswith(sandbox+'/data/diagnostics/')
    target=SCREENS/(name+'.png')
    device.scp(target,path,True)
    assert target.stat().st_size>2000

def test(smoke=False):
    report={'result':'FAIL','checks':[]}
    def check(label,condition):
        assert condition,label
        report['checks'].append(label)
        print('PASS '+label,flush=True)
    original=wait_for(lambda s:s['ready'] and not s['refreshing'])
    check('sandbox GUI and idle independent collector',original['euid']!=0 and not original['running'] and original['collector']['paused'])
    original_key=original['battery']['activeKey']
    other=next((b['key'] for b in original['batteries'] if b['key']!=original_key),None)
    try:
        before=backup('interval-deployed-before.sqlite')
        with sqlite3.connect(before) as db:
            count=db.execute('SELECT COUNT(*) FROM samples').fetchone()[0]
            check('additive migration integrity',db.execute('PRAGMA integrity_check').fetchone()[0]=='ok' and not db.execute('PRAGMA foreign_key_check').fetchall())
        capture('interval-idle')
        device.app_call('view',tab=1)
        capture('interval-results-empty')
        device.app_call('view',tab=0,batteries=True)
        capture('interval-markers')
        device.app_call('view',close=True)
        if other:
            device.app_call('use-battery',key=other)
            wait_for(lambda s:s['battery']['activeKey']==other)
            device.app_call('use-battery',key=original_key)
            returned=wait_for(lambda s:s['battery']['activeKey']==original_key)
            check('existing battery A/B/A marker identity',returned['batteries']==original['batteries'])
        after=backup('interval-idle-after.sqlite')
        with sqlite3.connect(after) as db:
            check('idle creates no battery samples',db.execute('SELECT COUNT(*) FROM samples').fetchone()[0]==count)
        if smoke:
            report['result']='PASS';return report
        device.app_call('start-test',minutes=1,interval=10)
        running=wait_for(lambda s:s['running'] and not s['pending'])
        test_id=running['test']['id']
        check('test starts and owns original battery',running['test']['battery_key']==original_key and running['test']['samples']>=1)
        capture('interval-running')
        device.stop_gui()
        print('Testing automatic stop while GUI is closed...',flush=True)
        # Wait for actual deadline, bounded at 55 seconds after screenshot/SSH overhead.
        remaining=max(0,60-running['test'].get('elapsed_s',0)-10)
        time.sleep(min(55,remaining)+3)
        device.launch()
        completed=wait_for(lambda s:not s['running'])
        report['test']=completed['test']
        check('test ends with GUI closed',completed['test']['id']==test_id and completed['test']['status'] in ('completed','interrupted'))
        check('automatic deadline and result persisted',completed['test']['reason']=='timer' and abs(completed['test']['elapsed_s']-60)<0.01)
        check('short test does not fabricate full capacity',not completed['test']['estimateReady'])
        device.app_call('select-test',id=test_id)
        wait_for(lambda s:s['detail'].get('id')==test_id)
        device.app_call('view',tab=1,result=True)
        capture('interval-result-detail')
        device.app_call('export')
        time.sleep(2)
        device.app_call('view',close=True,tab=0)
        final=backup('interval-after.sqlite')
        with sqlite3.connect(final) as db,sqlite3.connect(BUILD/'interval-before.sqlite') as old:
            for table in ['samples','sessions','snapshots','session_batteries','battery_profiles']:
                rows=old.execute('SELECT * FROM '+table+' ORDER BY 1').fetchall()
                retained=db.execute('SELECT * FROM '+table+' ORDER BY 1').fetchall()
                check('original '+table+' preserved',retained[:len(rows)]==rows)
            check('test sample ownership and isolation',db.execute('SELECT COUNT(*) FROM test_samples WHERE test_id=?',(test_id,)).fetchone()[0]==completed['test']['samples'] and db.execute('SELECT COUNT(*) FROM test_samples x JOIN samples s ON s.id=x.sample_id JOIN session_batteries b ON b.session_id=s.session_id WHERE x.test_id=? AND b.battery_key<>?',(test_id,original_key)).fetchone()[0]==0)
            check('final database integrity',db.execute('PRAGMA integrity_check').fetchone()[0]=='ok' and not db.execute('PRAGMA foreign_key_check').fetchall())
        report['result']='PASS';return report
    finally:
        now=state()
        if now.get('running'):
            device.app_call('stop-test')
            wait_for(lambda s:not s['running'] and not s['pending'])
        if now['battery']['activeKey']!=original_key:device.app_call('use-battery',key=original_key)
        device.app_call('view',tab=0,close=True)
        (BUILD/'interval-device-tests.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    test('--smoke' in sys.argv)
