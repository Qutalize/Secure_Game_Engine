"""Verify the metadata transformer against this project's real unsigned executable."""
import hashlib
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from audit_package import pe_info
from scrub_pe import scrub

ROOT=Path(__file__).resolve().parents[2]

class ScrubTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=ROOT/'output')
        self.addCleanup(self.temp.cleanup)
        self.path=Path(self.temp.name)/'game.exe'
        shutil.copyfile(ROOT/'build-protected/Release/GameEngine.exe',self.path)

    def test_code_and_security_flags_preserved(self):
        before=pe_info(self.path);data=self.path.read_bytes()
        sections={s['name']:data[s['raw_offset']:s['raw_offset']+s['raw_size']] for s in before['sections'] if s['executable']}
        self.assertGreater(before['debug_directory_bytes'],0)
        scrub(self.path);after=pe_info(self.path);data=self.path.read_bytes()
        self.assertEqual(after['debug_directory_bytes'],0)
        for s in after['sections']:
            if s['executable']:self.assertEqual(sections[s['name']],data[s['raw_offset']:s['raw_offset']+s['raw_size']])
        for field in ('entry_rva','sections','aslr','nx','cfg','high_entropy_aslr'):
            self.assertEqual(before[field],after[field])
        scrub(self.path);self.assertEqual(data,self.path.read_bytes())

    def test_refuses_signed_file_without_mutation(self):
        data=bytearray(self.path.read_bytes());nt=struct.unpack_from('<I',data,0x3c)[0]
        struct.pack_into('<II',data,nt+24+112+8*4,512,64)
        self.path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'signed'):scrub(self.path)
        self.assertEqual(data,self.path.read_bytes())

    def test_refuses_unknown_security_metadata_without_mutation(self):
        info=pe_info(self.path);data=bytearray(self.path.read_bytes());nt=struct.unpack_from('<I',data,0x3c)[0]
        rva=struct.unpack_from('<I',data,nt+24+112+8*6)[0]
        s=next(s for s in info['sections'] if s['rva']<=rva<s['rva']+s['raw_size'])
        struct.pack_into('<I',data,s['raw_offset']+rva-s['rva']+12,20)
        self.path.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'Unrecognized'):scrub(self.path)
        self.assertEqual(data,self.path.read_bytes())

if __name__=='__main__':unittest.main(verbosity=2)
