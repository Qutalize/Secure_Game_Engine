"""Compare recoverable C# IL, remaining metadata, and native disassembly in our Unity builds."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
from audit_package import pe_info

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('mono','il2cpp','ilspy','dumpbin','out'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=False)
    mono=args.mono.resolve()/'RangeUnity_Data/Managed/Assembly-CSharp.dll'
    native=args.il2cpp.resolve()/'GameAssembly.dll';meta=args.il2cpp.resolve()/'RangeUnity_Data/il2cpp_data/Metadata/global-metadata.dat'
    report={'targets':{name:{'file':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
        for name,p in [('mono',mono),('il2cpp',native),('metadata',meta)]}}
    for name,path in [('mono',mono),('il2cpp',native)]:
        result=subprocess.run(['dotnet',str(args.ilspy.resolve()),'-t','RangeComparison.Simulation',str(path),'-o',str(out/name)],
            capture_output=True,text=True,timeout=40)
        report[name+'_ilspy_exit']=result.returncode
        (out/(name+'-ilspy.log')).write_text(result.stdout+result.stderr,encoding='utf8')
    assert report['mono_ilspy_exit']==0 and report['il2cpp_ilspy_exit']!=0
    code=next((out/'mono').glob('*.cs')).read_text(encoding='utf8')
    report['mono_logic_readable']={name:name in code for name in ['void Step(','Config.speed','c.fire','current.nextShot','RayBox(','CanStand(']}
    assert all(report['mono_logic_readable'].values())
    metadata=meta.read_bytes();report['metadata_version']=struct.unpack_from('<I',metadata,4)[0]
    report['il2cpp_names_readable']={name:name.encode()+b'\0' in metadata for name in ['RangeComparison','Simulation','Step','CanStand','RayBox','Config']}
    report['il2cpp_clr_files']=[str(p.relative_to(args.il2cpp.resolve())) for p in args.il2cpp.resolve().rglob('Assembly-CSharp.dll')]
    data=native.read_bytes();info=pe_info(native);optional=struct.unpack_from('<I',data,0x3c)[0]+24
    base=struct.unpack_from('<Q',data,optional+24)[0]
    section=next((s for s in info['sections'] if s['executable'] and s['name']=='il2cpp'),next(s for s in info['sections'] if s['executable']))
    start=base+section['rva'];end=start+min(section['raw_size'],1024)
    with (out/'il2cpp-native-excerpt.txt').open('wb') as stream:
        subprocess.run([str(args.dumpbin.resolve()),'/DISASM',f'/RANGE:0x{start:X},0x{end:X}',str(native)],
            check=True,stdout=stream,stderr=subprocess.STDOUT,timeout=30)
    excerpt=(out/'il2cpp-native-excerpt.txt').read_text(encoding='utf8',errors='replace')
    report['native_instruction_lines']=len(re.findall(r'^\s*[0-9A-F]{16}:',excerpt,re.MULTILINE))
    report['excerpt_section']=section['name']
    assert report['native_instruction_lines']>10
    report['scope']='ILSpy only decompiles managed IL; its refusal is not a native reverse-engineering resistance score. Metadata names and machine instructions remain readable. Native pseudocode accuracy/effort unmeasured.'
    (out/'results.json').write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
