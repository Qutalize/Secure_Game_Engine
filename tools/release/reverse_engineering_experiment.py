"""Reproduce decompilation/disassembly observations on this project's own built games."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ilspy',type=Path,required=True,help='official ilspycmd.dll compatible with the installed dotnet runtime')
    parser.add_argument('--dumpbin',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    mono=ROOT/'output/packages/range-unity-mono-control/RangeUnity_Data/Managed/Assembly-CSharp.dll'
    native=ROOT/'output/packages/range-signed-v2-final/RangeGame.exe'
    report={'scope':'own binaries; recovery effort and accuracy are not quantified','targets':{}}
    for name,path in [('mono',mono),('signed_native',native)]:
        report['targets'][name]={'file':str(path),'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'bytes':path.stat().st_size}
    version=subprocess.run(['dotnet',str(args.ilspy),'--version'],check=True,capture_output=True,text=True,timeout=20)
    report['ilspy_version']=version.stdout.strip()
    result=subprocess.run(['dotnet',str(args.ilspy),'-t','RangeComparison.Simulation',str(mono),'-o',str(out/'mono')],
        capture_output=True,text=True,timeout=40)
    report['mono_exit_code']=result.returncode
    (out/'mono-tool.log').write_text(result.stdout+result.stderr,encoding='utf8')
    result.check_returncode()
    recovered=next((out/'mono').glob('*.cs'));code=recovered.read_text(encoding='utf8')
    markers=['void Step(','Config.speed','Config.tickRate','current.nextShot','c.fire','RayBox(','CanStand(']
    report['mono_recovered']={'file':str(recovered),'bytes':recovered.stat().st_size,'logic_markers':{s:s in code for s in markers}}
    assert all(report['mono_recovered']['logic_markers'].values()),'Expected own gameplay logic was not recovered'
    result=subprocess.run(['dotnet',str(args.ilspy),'-t','RangeComparison.Simulation',str(native),'-o',str(out/'native-as-csharp')],
        capture_output=True,text=True,timeout=40)
    (out/'native-ilspy.log').write_text(result.stdout+result.stderr,encoding='utf8')
    report['native_ilspy_exit_code']=result.returncode
    # ILSpy is a managed-code tool. Its refusal is not evidence of resistance to native decompilers.
    with (out/'native-assembly.txt').open('wb') as stream:
        subprocess.run([str(args.dumpbin),'/DISASM',str(native)],stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=40)
    assembly=(out/'native-assembly.txt').read_text(encoding='utf8',errors='replace')
    report['native_disassembly']={'bytes':(out/'native-assembly.txt').stat().st_size,
        'instruction_lines':len(re.findall(r'^\s*[0-9A-F]{16}:',assembly,re.MULTILINE)),
        'note':'Native machine instructions remain readable despite the signed inventory. Pseudocode recovery not measured.'}
    assert report['native_disassembly']['instruction_lines']>100
    (out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
