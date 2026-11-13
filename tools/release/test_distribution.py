"""Exercise the real CNG verifier against signed fixtures and altered package copies."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import unittest
import uuid
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
from sign_distribution import sign, HEADER

ROOT=Path(__file__).resolve().parents[2]
REPORT=[]

class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.folder=OUT/self._testMethodName
        self.folder.mkdir()
        (self.folder/'data').mkdir()
        (self.folder/'fixture.exe').write_bytes(b'owned test fixture, never executed')
        (self.folder/'data/rules.json').write_bytes(b'{"value":123}')
        sign(self.folder,ARGS.key,ARGS.public_key)

    def rewrite_manifest(self,body,key=None):
        if key is None:key=serialization.load_pem_private_key(ARGS.key.read_bytes(),password=None)
        r,s=utils.decode_dss_signature(key.sign(body,ec.ECDSA(hashes.SHA256())))
        (self.folder/'distribution.manifest').write_bytes(body)
        (self.folder/'distribution.sig').write_bytes(r.to_bytes(32,'big')+s.to_bytes(32,'big'))

    def probe(self,expected,folder=None):
        result=subprocess.run([str(ARGS.probe),str(folder or self.folder)],capture_output=True,text=True,timeout=15)
        REPORT.append(dict(test=self._testMethodName,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr))
        self.assertEqual(result.returncode,expected,result.stdout+result.stderr)
        self.assertIn('VERIFIED' if expected==0 else 'REJECTED',result.stdout+result.stderr)
        return result

    def test_valid_fixture(self):self.probe(0)
    def test_real_package(self):self.probe(0,ARGS.package)

    def test_modified_file(self):
        (self.folder/'data/rules.json').write_bytes(b'{"value":999}')
        self.assertIn('hash mismatch',self.probe(1).stderr)

    def test_truncated_file(self):
        (self.folder/'fixture.exe').write_bytes(b'cut')
        self.assertIn('size mismatch',self.probe(1).stderr)

    def test_missing_file(self):
        (self.folder/'fixture.exe').unlink()
        self.assertIn('inventory mismatch',self.probe(1).stderr)

    def test_extra_file(self):
        (self.folder/'extra.dll').write_bytes(b'not approved')
        self.assertIn('inventory mismatch',self.probe(1).stderr)

    def test_missing_metadata(self):
        (self.folder/'distribution.sig').unlink()
        self.probe(1)

    def test_signature_lengths(self):
        for size in (0,63,65):
            with self.subTest(size=size):
                (self.folder/'distribution.sig').write_bytes(bytes(size));self.probe(1)

    def test_invalid_signature(self):
        (self.folder/'distribution.sig').write_bytes(bytes(64))
        self.assertIn('signature rejected',self.probe(1).stderr)

    def test_wrong_key(self):
        self.rewrite_manifest((self.folder/'distribution.manifest').read_bytes(),ec.generate_private_key(ec.SECP256R1()))
        self.assertIn('signature rejected',self.probe(1).stderr)

    def test_changed_manifest(self):
        p=self.folder/'distribution.manifest';p.write_bytes(p.read_bytes().replace(b'123',b'999')+b'\n')
        self.assertIn('signature rejected',self.probe(1).stderr)

    def test_authenticated_unsafe_paths(self):
        for name in ('../outside','/absolute','data//bad','data/../bad','data/bad.','data/NUL.txt','data/COM1','C:/escape','data/a:b','data/back\\slash','distribution.sig','data/trailing/'):
            with self.subTest(name=name):
                self.rewrite_manifest(HEADER+b'0'*64+b' 0 '+name.encode()+b'\n');self.probe(1)

    def test_authenticated_malformed_records(self):
        for body in (b'',HEADER,HEADER+b'0'*64+b' 00 fixture.exe\n',HEADER+b'0'*64+b' -1 fixture.exe\n',
                     HEADER+b'0'*64+b' 536870913 fixture.exe\n',HEADER+b'G'*64+b' 0 fixture.exe\n',
                     HEADER+b'0'*64+b' 0 fixture.exe',b'WRONG 1\n'):
            with self.subTest(length=len(body)):
                self.rewrite_manifest(body);self.probe(1)

    def test_duplicate_casefold_path(self):
        self.rewrite_manifest(HEADER+b'0'*64+b' 0 fixture.exe\n'+b'0'*64+b' 0 FIXTURE.exe\n')
        self.assertIn('Duplicate',self.probe(1).stderr)

    def test_metadata_alias(self):
        p=self.folder/'distribution.manifest';p.rename(self.folder/'Distribution.Manifest')
        self.assertIn('canonical filename',self.probe(1).stderr)

    def test_reparse_directory(self):
        target=OUT/(self._testMethodName+'-target');target.mkdir()
        # Both paths are fresh directories under this run's evidence root.
        command=f"New-Item -ItemType Junction -Path '{self.folder/'junction'}' -Target '{target}' | Out-Null"
        subprocess.run(['powershell','-NoProfile','-Command',command],check=True,capture_output=True,timeout=15)
        self.assertIn('reparse point',self.probe(1).stderr)

    def test_read_locks(self):
        with subprocess.Popen([str(ARGS.probe),str(self.folder),'--hold'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True) as child:
            try:
                line=child.stdout.readline();self.assertIn('VERIFIED',line)
                for p in (self.folder/'fixture.exe',self.folder/'data/rules.json',self.folder/'distribution.manifest'):
                    with self.subTest(path=p.name):
                        with self.assertRaises(PermissionError):
                            with p.open('r+b') as stream:stream.write(b'X')
                        with self.assertRaises(PermissionError):p.unlink()
                self.assertEqual(child.wait(timeout=15),0)
                # Once the trusted verifier exits, ordinary file editing is possible again.
                with (self.folder/'fixture.exe').open('r+b') as stream:stream.write(b'X')
            finally:
                if child.poll() is None:child.kill();child.wait()

    def test_signer_wrong_key(self):
        another=OUT/'wrong-public.bin';another.write_bytes(bytes(64))
        fresh=OUT/'wrong-sign-output';fresh.mkdir();(fresh/'owned.exe').write_bytes(b'fixture')
        with self.assertRaisesRegex(ValueError,'does not match'):sign(fresh,ARGS.key,another)
        self.assertFalse((fresh/'distribution.sig').exists())

    def test_real_executable_modification(self):
        copy=OUT/'real-modified';shutil.copytree(ARGS.package,copy)
        exe=next(copy.glob('*.exe'))
        data=bytearray(exe.read_bytes());data[-1]^=1;exe.write_bytes(data)
        # Inspect with our trusted probe, never run a modified executable.
        self.assertIn('hash mismatch',self.probe(1,copy).stderr)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--package',type=Path,required=True)
    parser.add_argument('--key',type=Path,required=True);parser.add_argument('--public-key',type=Path,required=True)
    ARGS=parser.parse_args()
    for name in ('probe','package','key','public_key'):setattr(ARGS,name,getattr(ARGS,name).resolve())
    OUT=ROOT/'output/distribution-tests'/uuid.uuid4().hex;OUT.mkdir(parents=True)
    result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(DistributionTests))
    (OUT/'results.json').write_text(json.dumps(dict(tests=result.testsRun,successful=result.wasSuccessful(),
        failures=[(str(t),message) for t,message in result.failures+result.errors],observations=REPORT),indent=2),encoding='utf8')
    print('Evidence:',OUT)
    raise SystemExit(0 if result.wasSuccessful() else 1)
