"""Check packaged signed Player entry points, deterministic gameplay, and verification cost."""
import argparse
import json
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import uuid
ROOT=Path(__file__).resolve().parents[2]
TRACE=ROOT/'build-range/Release/range_trace.exe'
if not TRACE.is_file(): TRACE=ROOT/'output/tools/range_trace.exe'
sys.path.insert(0,str(ROOT/'comparison'))
from compare_traces import read,compare

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args();package=args.package.resolve();exe=next(package.glob('*.exe'))
    out=ROOT/'output/signed-player-tests'/uuid.uuid4().hex;out.mkdir(parents=True)
    report={};checks=[]
    def run(name,command,expected):
        with (out/(name+'.log')).open('wb') as log:
            result=subprocess.run([str(p) for p in command],cwd=out,stdout=log,stderr=subprocess.STDOUT,
                timeout=35,creationflags=subprocess.CREATE_NO_WINDOW)
        text=(out/(name+'.log')).read_text(encoding='utf8',errors='replace')
        checks.append(dict(name=name,exit_code=result.returncode,expected=expected))
        assert result.returncode==expected,(name,result.returncode,text)
        return text
    try:
        run('verify_only',[exe,'--verify-distribution'],0)
        run('legacy_server_rejected',[exe,'--server'],2)
        text=run('external_rules_rejected',[exe,'--mode','range','--range-data',ROOT/'comparison/data/range.json'],1)
        assert 'Range data must be part' in text and 'SECURITY START' not in text
        run('reference',[TRACE,ROOT/'comparison/data/range.json',
            ROOT/'comparison/data/victory.json',out/'reference.csv'],0)
        text=run('victory',[exe,'--mode','range','--replay',ROOT/'comparison/data/victory.json',
            '--state-out',out/'player.csv','--exit-after-replay'],0)
        assert 'DISTRIBUTION VERIFIED' in text
        report['gameplay']=compare(read(out/'reference.csv'),read(out/'player.csv'))
        # A damaged shader must be rejected before the game opens a renderer.
        damaged=out/'damaged';shutil.copytree(package,damaged)
        shader=next((damaged/'shaders').glob('*.dxil'));data=bytearray(shader.read_bytes());data[-1]^=1;shader.write_bytes(data)
        text=run('damaged_shader_rejected',[damaged/exe.name,'--mode','range'],1)
        assert 'hash mismatch' in text and 'SECURITY START' not in text
        timings=[]
        for _ in range(8):
            result=subprocess.run([str(args.probe.resolve()),str(package)],check=True,capture_output=True,text=True,timeout=15)
            timings.append(float(result.stdout.split('elapsed_ms=')[1]))
        report['verification_ms']={'warmup':timings[0],'samples':timings[1:],'median':statistics.median(timings[1:]),
            'minimum':min(timings[1:]),'maximum':max(timings[1:]),'cache':'uncontrolled; separate warmup excluded'}
        report['successful']=True
    finally:
        report['checks']=checks
        (out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf8')
        print('Evidence:',out)
    print(json.dumps(report,indent=2))
if __name__=='__main__':main()
