"""Generate, build, package and optionally launch a selected native creator runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from advisor import advise

ROOT = Path(__file__).resolve().parents[2]


def find_cmake(explicit=None):
    if explicit:
        path = Path(explicit).resolve()
        if not path.is_file():
            raise ValueError('CMake not found: ' + str(path))
        return str(path)
    found = shutil.which('cmake')
    if found:
        return found
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
    if vswhere.is_file():
        paths = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-find',
            'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'], text=True, encoding='utf-8').splitlines()
        if paths:
            return paths[0]
    raise ValueError('VS2022 C++/CMake tools are required. Pass --cmake if installed elsewhere.')


def safe_name(value):
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,63}', value) or value.upper() in {'CON','PRN','AUX','NUL',*(f'COM{i}' for i in range(10)),*(f'LPT{i}' for i in range(10))}:
        raise ValueError('--name must be a safe lowercase folder name, 1..64 characters')
    return value


def under_root(path):
    resolved = path.resolve()
    if not resolved.is_relative_to(ROOT.resolve()):
        raise ValueError('Output path escapes the project: ' + str(resolved))
    return resolved


def signing_identity(config, private_file, public_file):
    if config['distribution'] != 'signed':
        if private_file or public_file:
            raise ValueError('Signing keys supplied for an unsigned configuration')
        return None
    if not private_file or not public_file:
        raise ValueError('Signed builds require --signing-key and --public-key; keys are never generated implicitly')
    try:
        sys.path.insert(0,str(ROOT/'tools/release'))
        import sign_distribution
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError as exc:
        raise ValueError('Signing needs Python cryptography; select an interpreter with this dependency') from exc
    private_file=Path(private_file).resolve();public_file=Path(public_file).resolve()
    key=serialization.load_pem_private_key(private_file.read_bytes(),password=None)
    if not isinstance(key,ec.EllipticCurvePrivateKey) or not isinstance(key.curve,ec.SECP256R1):
        raise ValueError('Expected a P-256 signing key')
    public=public_file.read_bytes()
    if len(public)!=64 or sign_distribution.public_bytes(key)!=public:
        raise ValueError('Signing key does not match the public key')
    return {'private':private_file,'public':public_file,'fingerprint':hashlib.sha256(public).hexdigest(), 'sign':sign_distribution.sign}


def build(request, name=None, cmake=None, run=False, smoke=False, signing_key=None, public_key=None):
    result = advise(request)
    if result['native_build']['blockers']:
        raise ValueError('\n'.join(result['native_build']['blockers']))
    identity=signing_identity(result['request'],signing_key,public_key)
    from model_asset import cook
    model=cook(result['request'],ROOT)
    if model:result['native_build']['model_asset']=model['info']
    content = json.dumps(result['request'], ensure_ascii=True, sort_keys=True, indent=2)+'\n'
    digest = hashlib.sha256(content.encode()+(identity['fingerprint'].encode() if identity else b'')+(model['data']+(model.get('animation_data') or b'')+(model.get('rig_data') or b'') if model else b'')).hexdigest()[:12]
    name = safe_name(name or (result['request']['game']+'-'+digest))
    package = under_root(ROOT/'output/packages'/('selected-'+name))
    # Do not replace a signed package, user data, or an earlier build implicitly.
    if package.exists():
        raise ValueError('Package exists; choose a new --name: ' + str(package))
    cache = under_root(ROOT/'output/build-selected'/digest)
    cache.mkdir(parents=True, exist_ok=True)
    config = cache/'request.json'
    if not config.exists() or config.read_text(encoding='utf-8') != content:
        config.write_text(content, encoding='utf-8')
    cmake = find_cmake(cmake)
    commands = [
        [cmake,'-S',str(ROOT),'-B',str(cache),'-G','Visual Studio 17 2022','-A','x64',
         '-DRUNTIME_CONFIG='+str(config),'-DPython3_EXECUTABLE='+sys.executable,
         '-DPROTECT_DISTRIBUTION=ON','-DVERIFY_DISTRIBUTION='+('ON' if identity else 'OFF')] +
        (['-DDISTRIBUTION_PUBLIC_KEY_FILE='+str(identity['public'])] if identity else []),
        [cmake,'--build',str(cache),'--config','Release','--target','game','--parallel','2'],
        [cmake,'--install',str(cache),'--config','Release','--prefix',str(package)],
    ]
    log = cache/'last-build.log'
    with log.open('w',encoding='utf-8') as stream:
        for command in commands:
            print(' '.join(command), flush=True)
            stream.write(json.dumps(command,ensure_ascii=False)+'\n');stream.flush()
            done = subprocess.run(command,cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT)
            if done.returncode:
                raise ValueError('Build/install failed; see '+str(log))
    executable = package/'SelectedGame.exe'
    actual_shaders = sorted(p.stem for p in (package/'shaders').glob('*.dxil'))
    if actual_shaders != sorted(result['native_build']['shaders']):
        raise ValueError('Packaged shaders do not match the selection')
    if model:
        packaged_plan=json.loads((package/'runtime-plan.json').read_text(encoding='utf-8'))
        if model.get('rig_data') and (package/'assets/model.rig').read_bytes()!=model['rig_data']:
            raise ValueError('Ragdoll data changed during build')
        if model.get('animation_data') and (package/'assets/model.anim').read_bytes()!=model['animation_data']:
            raise ValueError('Animation changed during build; rebuild with a fresh package name')
        if (package/'assets/model.mesh').read_bytes()!=model['data'] or packaged_plan.get('model_asset')!=model['info']:
            raise ValueError('Model changed during build; rebuild with a fresh package name')
    (package/'request.json').write_text(content,encoding='utf-8')
    (package/'recommendations.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    files = {str(p.relative_to(package)).replace('\\','/'): {'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in sorted(package.rglob('*')) if p.is_file()}
    (package/'inventory.json').write_text(json.dumps({'signed':bool(identity),'files':files},indent=2)+'\n',encoding='utf-8')
    if identity:
        identity['sign'](package,identity['private'],identity['public'])
    described = subprocess.run([str(executable),'--describe'],cwd=package,capture_output=True,timeout=15,check=True)
    compiled = json.loads(described.stdout)
    if compiled['request'] != result['request'] or compiled['features'] != result['native_build']['features']:
        raise ValueError('Compiled configuration does not match the request')
    if model and compiled.get('model_asset')!=model['info']:
        raise ValueError('Compiled model metadata does not match the cooked source')
    if smoke:
        evidence = under_root(ROOT/'output/validation/selected'/name)
        evidence.mkdir(parents=True,exist_ok=True)
        with (evidence/'smoke.log').open('w',encoding='utf-8') as stream:
            done = subprocess.run([str(executable),'--smoke-test','--metrics',str(evidence/'startup.csv')],
                cwd=package,stdout=stream,stderr=subprocess.STDOUT,timeout=45)
        (evidence/'result.json').write_text(json.dumps({'exit_code':done.returncode,'package':str(package),
            'compiled':compiled,'bytes':sum(p.stat().st_size for p in package.rglob('*') if p.is_file())},indent=2)+'\n',encoding='utf-8')
        if done.returncode:
            raise ValueError('Runtime smoke failed; see '+str(evidence/'smoke.log'))
    print('Package: '+str(package),flush=True)
    if run:
        subprocess.run([str(executable)],cwd=package,check=True)
    return package


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--config',type=Path)
    group.add_argument('--interactive',action='store_true')
    parser.add_argument('--name',type=safe_name)
    parser.add_argument('--cmake')
    parser.add_argument('--run',action='store_true')
    parser.add_argument('--smoke-test',action='store_true')
    parser.add_argument('--signed',action='store_true',help='Select signed distribution (overrides the JSON field)')
    parser.add_argument('--signing-key',type=Path)
    parser.add_argument('--public-key',type=Path)
    args=parser.parse_args()
    try:
        if args.interactive:
            from advisor import interactive
            request=interactive()
        else:
            request=json.loads(args.config.read_text(encoding='utf-8-sig'))
        if args.signed:
            request['distribution']='signed'
        build(request,args.name,args.cmake,args.run,args.smoke_test,args.signing_key,args.public_key)
    except (OSError,ValueError,subprocess.SubprocessError) as exc:
        parser.exit(2,str(exc)+'\n')


if __name__=='__main__':
    main()
