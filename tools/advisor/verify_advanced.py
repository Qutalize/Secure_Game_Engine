"""Audit an already built/smoke-tested selected package; exercise signed startup on disposable copies."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from contextlib import contextmanager
from build_runtime import ROOT, under_root
from verify_runtime import verify_package


@contextmanager
def disposable(evidence):
    temp=under_root(Path(tempfile.mkdtemp(prefix='signature-test-',dir=evidence)))
    try:
        yield temp
    finally:
        # Windows can briefly retain executable mappings / scanner handles after process exit.
        for attempt in range(8):
            try:
                shutil.rmtree(temp)
                break
            except PermissionError:
                if attempt==7:raise
                time.sleep(min(.1*2**attempt,2))


def audit(package, evidence):
    package=package.resolve();evidence=under_root(evidence);evidence.mkdir(parents=True,exist_ok=True)
    dumps=sorted(Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC').glob('*/bin/Hostx64/x64/dumpbin.exe'))
    result={'package':str(package),'audit':verify_package(package,dumps[-1]),'cases':[]}
    def run(name,folder,args,expected):
        p=subprocess.run([str(folder/'SelectedGame.exe'),*args],cwd=folder,capture_output=True,timeout=45)
        log=p.stdout+p.stderr;(evidence/(name+'.log')).write_bytes(log)
        ok=p.returncode==expected and (expected==0 or b'SELECTED START' not in log)
        result['cases'].append({'name':name,'exit_code':p.returncode,'success':ok})
        if not ok:raise AssertionError(name+': '+log.decode('utf-8',errors='replace'))
        return log
    if result['audit']['features']['distribution_signature']:
        # Never mutate the delivered package. The temporary tree is wholly inside project output.
        original={p.relative_to(package).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in package.rglob('*') if p.is_file()}
        run('valid',package,['--verify-distribution'],0)
        run('valid-describe',package,['--describe'],0)
        run('valid-self-test',package,['--self-test'],0)
        for case in ('request','shader','executable','inventory','manifest','signature','missing-signature','missing-manifest','extra-file','foreign-signature','model','missing-model','animation','missing-animation','rig','missing-rig'):
            if case in ('model','missing-model') and not (package/'assets/model.mesh').is_file():continue
            if case in ('rig','missing-rig') and not (package/'assets/model.rig').is_file():continue
            if case in ('animation','missing-animation') and not (package/'assets/model.anim').is_file():continue
            if case=='shader' and not (package/'shaders').exists():continue
            with disposable(evidence) as temp:
                temp=under_root(Path(temp));copy=temp/'package';shutil.copytree(package,copy)
                if case=='missing-rig':(copy/'assets/model.rig').unlink()
                elif case=='rig':(copy/'assets/model.rig').write_bytes(b'corrupted rig')
                elif case=='missing-animation':(copy/'assets/model.anim').unlink()
                elif case=='animation':(copy/'assets/model.anim').write_bytes(b'corrupted animation')
                elif case=='missing-model':(copy/'assets/model.mesh').unlink()
                elif case=='model':(copy/'assets/model.mesh').write_bytes(b'corrupted geometry')
                elif case=='missing-signature':(copy/'distribution.sig').unlink()
                elif case=='missing-manifest':(copy/'distribution.manifest').unlink()
                elif case=='extra-file':(copy/'extra.dll').write_bytes(b'not executable')
                elif case=='foreign-signature':
                    from cryptography.hazmat.primitives import hashes
                    from cryptography.hazmat.primitives.asymmetric import ec,utils
                    key=ec.generate_private_key(ec.SECP256R1())
                    a,b=utils.decode_dss_signature(key.sign((copy/'distribution.manifest').read_bytes(),ec.ECDSA(hashes.SHA256())))
                    (copy/'distribution.sig').write_bytes(a.to_bytes(32,'big')+b.to_bytes(32,'big'))
                else:
                    names={'request':'request.json','executable':'SelectedGame.exe','inventory':'inventory.json','manifest':'distribution.manifest','signature':'distribution.sig'}
                    target=next((copy/'shaders').glob('*.dxil')) if case=='shader' else copy/names[case]
                    target.write_bytes(target.read_bytes()+b'X')
                run('reject-'+case,copy,['--describe'],2)
        for name,path in [('inside',package/'new-metrics.csv'),('case-alias',Path(str(package).swapcase())/'new-metrics.csv')]:
            log=run('metrics-'+name,package,['--smoke-test','--metrics',str(path)],2)
            assert b'Metrics must be outside' in log
        after={p.relative_to(package).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in package.rglob('*') if p.is_file()}
        assert original==after,'Original package changed during tests'
    result['success']=True
    (evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('package',type=Path);args=parser.parse_args()
    result=audit(args.package,ROOT/'output/validation/selected/advanced'/args.package.name)
    print(json.dumps(result,indent=2))
