"""Save a bounded protocol observation and print only its useful summary."""
import argparse
import datetime
import json
import pathlib
import re
import device

def record(task_id,seconds=0):
    result=device.wait(task_id,seconds)
    folder=device.BUILD/'verification-0.1.0.12'
    path=folder/('task-'+task_id+'-'+datetime.datetime.now(datetime.timezone.utc).strftime('%H%M%S%f')+'.json')
    device.save(path,result,exclusive=True)
    state=result['state'];summary=dict(id=task_id,status=state['status'],message=state['message'],evidence=str(path))
    if 'result' in result:
        body=result['result']
        if isinstance(body,dict) and 'evidence' in body:
            matches=re.findall(r'(?m)^@@Q10_UNSIGNED_OBSERVATION (.+)$',body['evidence'])
            if matches:
                observation=json.loads(matches[-1]);summary.update({key:observation.get(key) for key in ('version','ready','serviceUid','managementUid')})
        elif isinstance(body,list):summary['entries']=len(body)
        elif isinstance(body,dict):
            summary['result']={key:body[key] for key in ('path','version','sha256','directoryName','action','outcome','originalDeploymentId','size','totalEntries','truncated') if key in body}
    print(json.dumps(summary,ensure_ascii=True,indent=2))
    return result
if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('id');parser.add_argument('--seconds',type=int,default=0)
    args=parser.parse_args();record(args.id,args.seconds)
