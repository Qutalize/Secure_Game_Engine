"""Inspect own PE files and reject source/debug/credential files in a client package."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

FORBIDDEN={'.pdb','.ilk','.exp','.lib','.cpp','.c','.h','.hpp','.cs','.rs','.hlsl','.key','.pem','.map','.sln','.vcxproj'}

def pe_info(path):
    data=path.read_bytes()
    if data[:2]!=b'MZ':raise ValueError('Not a PE file')
    offset=struct.unpack_from('<I',data,0x3c)[0]
    if data[offset:offset+4]!=b'PE\0\0':raise ValueError('Bad PE signature')
    coff=offset+4;machine,nsections,_,_,nsymbols,optional_size,_=struct.unpack_from('<HHIIIHH',data,coff)
    optional=coff+20;magic=struct.unpack_from('<H',data,optional)[0]
    if magic!=0x20b:raise ValueError('Expected PE32+')
    dllflags=struct.unpack_from('<H',data,optional+70)[0]
    directories=optional+112
    debug_rva,debug_size=struct.unpack_from('<II',data,directories+8*6)
    clr_rva,clr_size=struct.unpack_from('<II',data,directories+8*14)
    entry=struct.unpack_from('<I',data,optional+16)[0]
    sections=[]
    for i in range(nsections):
        o=optional+optional_size+40*i
        name=data[o:o+8].split(b'\0')[0].decode('ascii',errors='replace')
        virtual_size,rva,size,raw=struct.unpack_from('<IIII',data,o+8)
        flags=struct.unpack_from('<I',data,o+36)[0]
        sections.append(dict(name=name,rva=rva,virtual_size=virtual_size,raw_size=size,raw_offset=raw,executable=bool(flags&0x20000000),writable=bool(flags&0x80000000)))
    strings=[m.group().decode('ascii') for m in re.finditer(rb'[\x20-\x7e]{6,}',data)]
    debug_types=[]
    if debug_size:
        for section in sections:
            if section['rva']<=debug_rva<section['rva']+section['raw_size']:
                raw=section['raw_offset']+debug_rva-section['rva']
                debug_types=[struct.unpack_from('<I',data,raw+i+12)[0] for i in range(0,debug_size,28)]
                break
    source_hints=[s for s in strings if re.search(r'(?i)([A-Z]:[\\/].*\.(cpp|h|hlsl)|\.pdb\b)',s)]
    return dict(file=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),machine=machine,entry_rva=entry,
        coff_symbols=nsymbols,debug_directory_bytes=debug_size,debug_types=debug_types,managed_clr=bool(clr_rva or clr_size),
        aslr=bool(dllflags&0x40),nx=bool(dllflags&0x100),high_entropy_aslr=bool(dllflags&0x20),cfg=bool(dllflags&0x4000),
        ascii_string_count=len(strings),source_path_hint_count=len(source_hints),source_path_hint_examples=source_hints[:8],
        sections=sections)

def audit(folder):
    files=sorted(p for p in folder.rglob('*') if p.is_file());problems=[];executables=[]
    for path in files:
        relative=path.relative_to(folder)
        if path.suffix.lower() in FORBIDDEN or path.name.lower() in {'cheat_tester.exe','game_cheat.dll'}:
            problems.append('Forbidden distribution file: '+str(relative))
        if path.suffix.lower() in {'.exe','.dll'}:
            info=pe_info(path);executables.append(info)
            if info['debug_directory_bytes'] or info['coff_symbols'] or info['source_path_hint_count']:
                problems.append('Reverse-engineering metadata remains: '+str(relative))
            if not all(info[k] for k in ('aslr','nx','high_entropy_aslr','cfg')):problems.append('Missing ASLR/NX/CFG: '+str(relative))
            if info['managed_clr']:problems.append('Unexpected managed code in native package: '+str(relative))
    if not executables:problems.append('No executable files in package')
    return dict(files=len(files),total_bytes=sum(p.stat().st_size for p in files),executables=executables,problems=problems,
                note='This checks metadata and packaging. Native code and DXIL can still be disassembled.')

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('path',type=Path);parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--compare',type=Path);args=parser.parse_args()
    result=audit(args.path) if args.path.is_dir() else pe_info(args.path)
    if args.compare:result={'baseline':pe_info(args.compare),'protected':result}
    args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf8')
    print(args.out);return 1 if result.get('problems') else 0
if __name__=='__main__':raise SystemExit(main())
