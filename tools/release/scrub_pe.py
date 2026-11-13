"""Remove debug/POGO hints from an unsigned own PE32+ copy, without changing code sections."""
import argparse
import struct
from pathlib import Path
from audit_package import pe_info

def scrub(path):
    info=pe_info(path)
    if info['managed_clr']:raise ValueError('Managed files are not supported')
    data=bytearray(path.read_bytes());nt=struct.unpack_from('<I',data,0x3c)[0];optional=nt+24;directory=optional+112
    if any(struct.unpack_from('<II',data,directory+8*4)):raise ValueError('Do not modify signed files; scrub before signing')
    rva,size=struct.unpack_from('<II',data,directory+8*6)
    if not size:return
    if size%28:raise ValueError('Invalid debug directory size')
    def readonly_region(offset,size):
        if not any(not s['executable'] and not s['writable'] and s['name']=='.rdata' and s['raw_offset']<=offset and offset+size<=s['raw_offset']+s['raw_size'] for s in info['sections']):
            raise ValueError('Debug data outside expected read-only section')
    section=next(s for s in info['sections'] if s['rva']<=rva< s['rva']+s['raw_size'])
    raw=section['raw_offset']+rva-section['rva'];readonly_region(raw,size)
    regions=[]
    for i in range(0,size,28):
        kind,length,_,pointer=struct.unpack_from('<IIII',data,raw+i+12)
        # Do not drop unknown/security-bearing debug records (e.g. extended DLL characteristics).
        if kind not in {2,12,13,14,16}:raise ValueError(f'Unrecognized debug metadata type {kind}')
        if length:readonly_region(pointer,length);regions.append((pointer,length))
    for pointer,length in regions:data[pointer:pointer+length]=bytes(length)
    data[raw:raw+size]=bytes(size);data[directory+8*6:directory+8*7]=bytes(8)
    # Unsigned user-mode executable: checksum is optional; clear the stale value.
    data[optional+64:optional+68]=bytes(4)
    path.write_bytes(data)
    after=pe_info(path)
    if after['debug_directory_bytes']:raise ValueError('Metadata removal failed')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('exe',type=Path);a=p.parse_args();scrub(a.exe);print(a.exe)
