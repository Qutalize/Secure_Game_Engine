"""Sign our native distribution inventory with an offline P-256 key. This is not Authenticode."""
import argparse
import hashlib
from pathlib import Path
import re
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import ec,utils

HEADER=b'GE-DISTRIBUTION 1\n'
METADATA={'distribution.manifest','distribution.sig'}

def public_bytes(key):
    n=key.public_key().public_numbers()
    return n.x.to_bytes(32,'big')+n.y.to_bytes(32,'big')

def keygen(directory):
    directory.mkdir(parents=True,exist_ok=False)
    key=ec.generate_private_key(ec.SECP256R1())
    (directory/'release-private.key').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    (directory/'release-public.bin').write_bytes(public_bytes(key))
    print('Created an experimental signing identity. Keep release-private.key on the trusted build machine.')

def valid_path(name):
    if not re.fullmatch(r'[A-Za-z0-9_.\-/]+',name) or len(name)>240:raise ValueError('Unsupported package path: '+name)
    for part in name.split('/'):
        if not part or part in ('.','..') or part.endswith('.') or part.split('.')[0].upper() in {'CON','PRN','AUX','NUL',*[f'COM{i}' for i in range(1,10)],*[f'LPT{i}' for i in range(1,10)]}:
            raise ValueError('Unsafe package path: '+name)

def manifest(folder):
    entries=[];seen=set()
    for p in sorted(folder.rglob('*')):
        # Windows junctions/reparse points must not escape the inventory root.
        if p.is_symlink() or getattr(p.lstat(),'st_file_attributes',0)&0x400:raise ValueError('Reparse point in package')
        if not p.is_file():continue
        name=p.relative_to(folder).as_posix()
        if name in METADATA:continue
        if name.lower() in METADATA:raise ValueError('Reserved metadata filename alias')
        valid_path(name)
        if name.lower() in seen:raise ValueError('Case-insensitive duplicate path')
        seen.add(name.lower());size=p.stat().st_size
        if size>512*1024*1024:raise ValueError('Individual file exceeds 512 MiB limit')
        with p.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
        entries.append(f'{digest} {size} {name}\n'.encode('ascii'))
    if not entries or len(entries)>4096:raise ValueError('Invalid inventory size')
    body=HEADER+b''.join(entries)
    if len(body)>512*1024:raise ValueError('Manifest exceeds limit')
    return body

def sign(folder,private_file,expected_public):
    folder=folder.resolve();private_file=private_file.resolve()
    if private_file.is_relative_to(folder):raise ValueError('Private key must be outside the distribution')
    for name in METADATA:
        if (folder/name).exists():raise ValueError('Refusing to overwrite an existing signature/manifest')
    key=serialization.load_pem_private_key(private_file.read_bytes(),password=None)
    if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1):raise ValueError('Expected P-256 key')
    if public_bytes(key)!=expected_public.read_bytes():raise ValueError('Signing key does not match the build public key')
    body=manifest(folder);der=key.sign(body,ec.ECDSA(hashes.SHA256()));r,s=utils.decode_dss_signature(der)
    (folder/'distribution.manifest').write_bytes(body)
    (folder/'distribution.sig').write_bytes(r.to_bytes(32,'big')+s.to_bytes(32,'big'))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);sub=p.add_subparsers(dest='action',required=True)
    k=sub.add_parser('keygen');k.add_argument('directory',type=Path)
    s=sub.add_parser('sign');s.add_argument('folder',type=Path);s.add_argument('--key',type=Path,required=True);s.add_argument('--public-key',type=Path,required=True)
    a=p.parse_args()
    if a.action=='keygen':keygen(a.directory)
    else:sign(a.folder,a.key,a.public_key)
