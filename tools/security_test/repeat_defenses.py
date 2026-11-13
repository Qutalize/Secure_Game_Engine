"""Randomized repeated FPS defense measurements. Run without builds/other games."""
import argparse
import csv
import hashlib
import json
import platform
import random
import statistics
import subprocess
import sys
import time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/benchmark'))
from run import child_error_mode,qpc_clock,read_trace,percentile

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--game',type=Path,default=ROOT/'build-event/Release/GameEngine.exe')
    p.add_argument('--output',type=Path,required=True);p.add_argument('--runs',type=int,default=7);p.add_argument('--seconds',type=float,default=5)
    a=p.parse_args()
    if a.runs<3 or not 2<=a.seconds<=60:p.error('Use at least 3 runs and 2..60 seconds')
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);exe=a.game.resolve()
    features=['shuffle','watchdog','depth-guard','tag-mask'];policies=['none',*features,','.join(features)]
    now,freq=qpc_clock();rows=[];rng=random.Random(20261004)
    (out/'environment.json').write_text(json.dumps(dict(host=platform.node(),os=platform.platform(),exe=str(exe),
        exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),runs=a.runs,seconds=a.seconds,warmup_seconds=2,
        conditions='Stationary default FPS, 1600x900, DX12, VSync ON; randomized order; OS cache uncontrolled; no input-latency claim'),indent=2),encoding='utf8')
    for round_id in range(a.runs):
        order=policies.copy();rng.shuffle(order)
        for policy in order:
            name=f'{round_id:02d}-{policies.index(policy)}';trace=out/(name+'-frames.csv');cost=out/(name+'-cost.csv')
            command=[str(exe),'--mode','fps','--security',policy,'--security-report',str(cost),'--metrics',str(trace),
                '--benchmark-seconds',str(a.seconds),'--benchmark-warmup','2']
            row=dict(round=round_id,policy=policy,status='failed')
            try:
                with (out/(name+'.log')).open('wb') as log:
                    start=now()
                    with child_error_mode():child=subprocess.Popen(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
                    try:code=child.wait(timeout=a.seconds+35)
                    except BaseException:child.kill();child.wait();raise
                if code:raise RuntimeError(f'Exit {code}')
                row['trace']=read_trace(trace,start,freq,a.seconds)
                costs=list(csv.DictReader(cost.open(newline='',encoding='utf8')))
                for c in costs:
                    if (int(c['calls'])>0)!=(c['feature'] in policy.split(',')):raise RuntimeError('Unexpected feature calls')
                row.update(status='ok',costs=costs)
            except Exception as e:row['error']=str(e)
            rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2),encoding='utf8')
            print(name,policy,row['status'],flush=True);time.sleep(.4)
    summary={}
    for policy in policies:
        group=[r for r in rows if r['policy']==policy];valid=[r for r in group if r['status']=='ok']
        item=dict(attempts=len(group),valid=len(valid),failed=len(group)-len(valid))
        if valid:
            numeric={k for k,v in valid[0]['trace'].items() if isinstance(v,(int,float))}
            item['metrics']={key:{'median':statistics.median([r['trace'][key] for r in valid]),
                'min':min(r['trace'][key] for r in valid),'max':max(r['trace'][key] for r in valid)} for key in sorted(numeric)}
            item['scope_mean_ns']={feature:statistics.median([float(next(c for c in r['costs'] if c['feature']==feature)['elapsed_mean_ns']) for r in valid]) for feature in features}
        summary[policy]=item
    (out/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf8')
    return int(any(r['status']!='ok' for r in rows))
if __name__=='__main__':raise SystemExit(main())
