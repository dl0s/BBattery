"""Repeated Q10 launches of the simplified interval-test UI."""
import json,pathlib,re,sys,time
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import device

def test():
    report={'result':'FAIL','launches':[]}
    receipt,_,sandbox=device.receipt()
    try:
        for attempt in range(1,4):
            device.stop_gui()
            started=time.monotonic()
            device.launch()
            state=device.app_call('state')
            assert state['ready'] and state['euid']!=0 and not state['running']
            loaded=time.monotonic()-started
            log=device.ssh('cat '+device.quote(sandbox+'/logs/log'))
            assert 'BBattery '+receipt['version']+' native scene ready' in log
            assert 'SIGSEGV' not in log and 'QML:' not in log
            timing=re.search(r'BBattery scene prepared: (\d+) ms',log)
            assert timing
            device.app_call('view',tab=1)
            device.app_call('view',tab=0)
            report['launches'].append({'attempt':attempt,'sceneMs':int(timing.group(1)),'dataSeconds':loaded,'readMs':state['readMs']})
            print('PASS launch '+str(attempt)+' scene='+timing.group(1)+' ms indexedRead='+str(state['readMs'])+' ms',flush=True)
        report['result']='PASS'
    finally:
        (ROOT/'build/startup-tests.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
if __name__=='__main__':test()
