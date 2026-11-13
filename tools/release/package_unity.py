"""Stage this project's IL2CPP range, preserving raw/debug output outside the shipped folder."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
from audit_package import pe_info
from scrub_pe import scrub

EXE='RangeUnity.exe'
DATA='RangeUnity_Data'
METADATA=Path(DATA)/'il2cpp_data/Metadata/global-metadata.dat'
TOP_FILES={EXE,'GameAssembly.dll','UnityPlayer.dll','UnityCrashHandler64.exe','baselib.dll'}
PRIVATE_SUFFIXES={'.pdb','.mdb','.ilk','.exp','.lib','.obj','.cpp','.c','.h','.hpp','.cs','.map','.sln','.vcxproj','.key','.pem'}

def category(relative):
    parts=relative.parts
    if any('butdontshipitwithyourgame' in p.lower() or 'donotship' in p.lower() for p in parts):return 'private-debug'
    if relative.suffix.lower() in PRIVATE_SUFFIXES or relative.name.lower()=='symbolmap':return 'private-debug'
    if any(p.lower() in {'managed','monobleedingedge'} for p in parts):raise ValueError('Managed/Mono output is not an IL2CPP package: '+str(relative))
    if len(parts)==1 and relative.name in TOP_FILES:return 'runtime'
    if len(parts)==2 and parts[0]=='D3D12' and relative.name in {'D3D12Core.dll','d3d12SDKLayers.dll'}:return 'runtime'
    if len(parts)>1 and parts[0]==DATA:return 'runtime'
    raise ValueError('Unreviewed Unity build file: '+str(relative))

def sha(path):
    with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def code_hashes(path):
    info=pe_info(path);data=path.read_bytes()
    return {s['name']:hashlib.sha256(data[s['raw_offset']:s['raw_offset']+s['raw_size']]).hexdigest()
        for s in info['sections'] if s['executable']}

def audit(folder):
    problems=[];files=[];native=[]
    for name in [EXE,'UnityPlayer.dll','GameAssembly.dll',METADATA.as_posix()]:
        if not (folder/name).is_file():problems.append('Missing required IL2CPP runtime file: '+name)
    for path in sorted(folder.rglob('*')):
        if path.is_symlink() or getattr(path.lstat(),'st_file_attributes',0)&0x400:
            problems.append('Reparse point: '+str(path.relative_to(folder)));continue
        if not path.is_file():continue
        relative=path.relative_to(folder)
        try:
            if category(relative)!='runtime':problems.append('Private build data present: '+str(relative))
        except ValueError as e:problems.append(str(e))
        files.append({'path':relative.as_posix(),'bytes':path.stat().st_size,'sha256':sha(path)})
        if path.suffix.lower() in {'.exe','.dll'}:
            info=pe_info(path);info['file']=relative.as_posix();native.append(info)
            if info['managed_clr']:problems.append('CLR IL assembly in protected output: '+str(relative))
    metadata={}
    if (folder/METADATA).exists():
        data=(folder/METADATA).read_bytes()
        if len(data)<8:problems.append('Truncated IL2CPP metadata')
        else:
            magic,version=struct.unpack_from('<II',data)
            if magic!=0xFAB11BAF:problems.append('Invalid IL2CPP metadata magic')
            metadata={'version':version,'bytes':len(data),'sha256':sha(folder/METADATA),
                'own_names_readable':{name:name.encode()+b'\0' in data for name in ['RangeComparison','Simulation','CanStand','RayBox','Step','Config']}}
    return {'files':files,'file_count':len(files),'total_bytes':sum(f['bytes'] for f in files),'native':native,
        'metadata':metadata,'problems':problems,
        'scope':'Packaging/metadata inspection, not proof against native decompilation. Unity vendor binaries are unmodified; application distribution-signature enforcement is not implemented here.'}

def stage(source,out):
    source=source.resolve();out=out.resolve()
    if out==source or out.is_relative_to(source):raise ValueError('Stage outside the raw build')
    if out.exists():raise ValueError('Use a new output directory')
    if not (source/'GameAssembly.dll').is_file() or not (source/METADATA).is_file():raise ValueError('Expected a completed IL2CPP build, not Mono')
    selected=[];excluded=[]
    for path in sorted(source.rglob('*')):
        if path.is_symlink() or getattr(path.lstat(),'st_file_attributes',0)&0x400:raise ValueError('Reparse point in raw build')
        if not path.is_file():continue
        relative=path.relative_to(source)
        if category(relative)=='runtime':selected.append((path,relative))
        else:excluded.append({'path':relative.as_posix(),'bytes':path.stat().st_size})
    out.mkdir(parents=True)
    for path,relative in selected:
        destination=out/relative;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,destination)
    # Only our generated, unsigned application DLL is scrubbed. Preserve Unity's supplied binaries.
    application=out/'GameAssembly.dll';before=code_hashes(application);raw_info=pe_info(application)
    scrub(application)
    if before!=code_hashes(application):raise ValueError('Executable section changed during metadata removal')
    report=audit(out)
    report['raw_application']=raw_info
    report['excluded_private_files']=excluded
    report['executable_sections_unchanged']=True
    report['raw_build_preserved']=str(source)
    report_path=out.with_name(out.name+'-audit.json');report_path.write_text(json.dumps(report,indent=2),encoding='utf8')
    if report['problems']:raise ValueError('Audit failed; see '+str(report_path))
    print(json.dumps({'package':str(out),'files':report['file_count'],'bytes':report['total_bytes'],
        'excluded_files':len(excluded),'metadata':report['metadata'],'problems':report['problems']},indent=2))
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();stage(args.source,args.out)
if __name__=='__main__':main()
