"""Compare protected native game state to the normal build and verify package-only shader loading."""
import json
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'comparison'))
from compare_traces import compare,read

def main():
    out=ROOT/'output/release-validation/range';out.mkdir(parents=True,exist_ok=False);report={}
    for scenario in ('validation','victory','tour'):
        normal=out/f'normal-{scenario}.csv';protected=out/f'protected-{scenario}.csv'
        for build,dest in [('build-range',normal),('build-range-protected',protected)]:
            subprocess.run([str(ROOT/build/'Release/range_trace.exe'),str(ROOT/'comparison/data/range.json'),
                            str(ROOT/f'comparison/data/{scenario}.json'),str(dest)],check=True,capture_output=True)
        report[scenario]=compare(read(normal),read(protected))
    package=ROOT/'output/packages/range-protected-final'
    # Launch from an unrelated directory. Relative shader/data paths must resolve beside the executable.
    with (out/'player.log').open('wb') as log:
        subprocess.run([str(package/'RangeGame.exe'),'--replay',str(package/'data/victory.json'),'--state-out',str(out/'player.csv'),
                        '--exit-after-replay'],cwd=out,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
    report['packaged_player']=compare(read(out/'normal-victory.csv'),read(out/'player.csv'))
    (out/'parity.json').write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
