"""Run real Unity Players and compare every gameplay tick to the native reference."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
TRACE=ROOT/'build-range/Release/range_trace.exe'
if not TRACE.is_file(): TRACE=ROOT/'output/tools/range_trace.exe'
sys.path.insert(0,str(ROOT/'comparison'))
from compare_traces import read,compare

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mono',type=Path,required=True);parser.add_argument('--il2cpp',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    report={'runs':[],'successful':False}
    try:
        for scenario in ('victory','validation','tour'):
            reference=out/f'native-{scenario}.csv'
            subprocess.run([str(TRACE),str(ROOT/'comparison/data/range.json'),
                str(ROOT/f'comparison/data/{scenario}.json'),str(reference)],check=True,capture_output=True,timeout=15)
            players=[('IL2CPP',args.il2cpp)]
            if scenario=='victory':players.insert(0,('Mono',args.mono))
            for backend,exe in players:
                name=f'{backend.lower()}-{scenario}';trace=out/(name+'.csv');log=out/(name+'.log')
                row={'backend':backend,'scenario':scenario,'status':'failed'};report['runs'].append(row)
                with (out/(name+'-stdout.log')).open('wb') as stream:
                    result=subprocess.run([str(exe.resolve()),'-force-d3d11','-logFile',str(log),'--replay',str(ROOT/f'comparison/data/{scenario}.json'),
                        '--state-out',str(trace),'--exit-after-replay'],cwd=out,stdout=stream,stderr=subprocess.STDOUT,timeout=100,creationflags=subprocess.CREATE_NO_WINDOW)
                row['exit_code']=result.returncode
                text=log.read_text(encoding='utf8',errors='replace')
                assert result.returncode==0,(name,result.returncode)
                assert 'RANGE BACKEND: '+backend in text,name
                assert 'api=Direct3D11' in text and 'resolution=1600x900' in text,name
                assert 'Range setup failed' not in text and 'Exception:' not in text,name
                row.update(compare(read(reference),read(trace)));row['status']='ok'
                print(name+': '+str(row['ticks'])+' ticks match',flush=True)
        report['successful']=True
    finally:
        (out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf8');print('Evidence:',out,flush=True)
    return 0
if __name__=='__main__':raise SystemExit(main())
