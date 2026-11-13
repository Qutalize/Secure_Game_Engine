"""Check that each selected defense actually runs independently in real game modes."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/benchmark'))
from run import child_error_mode, qpc_clock, read_trace

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',type=Path,default=ROOT/'build-event/Release/GameEngine.exe')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    now,frequency=qpc_clock();features=['shuffle','watchdog','depth-guard','tag-mask'];results=[]
    for mode in ['fps','card','hide']:
        for mask in range(4 if mode=='card' else 16):
            selected=[name for bit,name in enumerate(features) if mask&(1<<bit)]
            policy=','.join(selected) or 'none';stem=f'{mode}-{mask:02d}'
            report=out/(stem+'-security.csv');trace=out/(stem+'-frames.csv')
            command=[str(args.game.resolve()),'--mode',mode,'--security',policy,'--security-report',str(report),
                '--metrics',str(trace),'--benchmark-seconds','1.1','--benchmark-warmup','0']
            row={'mode':mode,'policy':policy,'status':'failed'}
            try:
                with (out/(stem+'.log')).open('wb') as log:
                    launched=now()
                    with child_error_mode(): child=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,
                        creationflags=subprocess.CREATE_NO_WINDOW)
                    try: code=child.wait(timeout=30)
                    except BaseException:
                        child.kill();child.wait();raise
                if code: raise RuntimeError(f'exit={code}')
                row['trace']=read_trace(trace,launched,frequency,1.1)
                with report.open(newline='',encoding='utf8') as f: costs=list(csv.DictReader(f))
                if {c['feature'] for c in costs}!=set(features): raise RuntimeError('Missing cost rows')
                for cost in costs:
                    if (int(cost['calls'])>0)!=(cost['feature'] in selected):
                        raise RuntimeError('Unexpected active/inactive feature: '+str(cost))
                row.update(status='ok',costs=costs)
            except Exception as e: row['error']=str(e)
            results.append(row)
            (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
            print(stem,policy,row['status'],row.get('error',''),flush=True)
    return 1 if any(r['status']!='ok' for r in results) else 0

if __name__=='__main__':raise SystemExit(main())
