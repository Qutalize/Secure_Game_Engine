"""Build and run the selected-runtime matrix on a Windows development PC (no external downloads)."""
from concurrent.futures import ThreadPoolExecutor, as_completed
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from build_runtime import ROOT


def verify_package(package, dumpbin):
    config=json.loads((package/'request.json').read_text(encoding='utf-8'))
    plan=json.loads((package/'runtime-plan.json').read_text(encoding='utf-8'))
    exe=package/'SelectedGame.exe'
    imports=subprocess.check_output([str(dumpbin),'/imports',str(exe)]).decode('utf-8',errors='replace').lower()
    dlls=sorted(set(re.findall(r'\b[\w.-]+\.dll\b',imports)))
    features=plan['features'];binary=exe.read_bytes()
    assert ('d3d12.dll' in dlls)==features['render'],dlls
    # Windows SDK's XAudio2 import library loads XAudio2 dynamically; it may not appear in PE imports.
    dynamic_audio = 'xaudio2_9.dll'.encode('utf-16le') in binary.lower() or b'xaudio2_9.dll' in binary.lower()
    assert (any('xaudio' in dll for dll in dlls) or dynamic_audio)==features['audio'],dlls
    assert 'ws2_32.dll' not in dlls,dlls
    for enabled,names in [(features['pbr'],['PbrVS.dxil','PbrPS.dxil']),
                          (features['shadows'],['ShadowVS.dxil']),
                          (False,['TexturedVS.dxil','PbrSkinnedVS.dxil','ShadowSkinnedVS.dxil'])]:
        if not enabled:
            assert all(name.encode() not in binary for name in names),names
    inputs=(package/'build-inputs.txt').read_text(encoding='utf-8')
    assert ('Collision.cpp' in inputs)==features['collision']
    assert ('AudioPlayer.cpp' in inputs)==features['audio']
    assert ('Renderer.cpp' in inputs)==features['render']
    assert ('RigidBodyWorld.cpp' in inputs)==features.get('rigid_body',False)
    assert ('RayTracing.cpp' in inputs)==features.get('ray_tracing',False)
    assert (b'JOLT CHECK OK' in binary)==features.get('rigid_body',False)
    assert (b'RAY TRACING backend=' in binary)==features.get('ray_tracing',False)
    assert ('bcrypt.dll' in dlls)==features.get('distribution_signature',False)
    for flag,source,marker in [('model_geometry','TriangleMesh.cpp',b'Packaged model.mesh is missing'),
                               ('rt_mesh','TriangleAcceleration.cpp',b'RT MESH registered'),
                               ('model_convex',None,b'MODEL PHYSICS convex-hull'),
                               ('model_static',None,b'MODEL PHYSICS static-mesh'),
                               ('model_deforming','DeformingMeshCollider.cpp',b'MODEL PHYSICS deforming-mesh'),
                               ('ragdoll','RagdollModel.cpp',b'RAGDOLL active bones='),
                               ('deformation_ccd','ContinuousDeformation.cpp',b'DEFORMATION CCD active'),
                               ('model_animation','SkeletalAnimation.cpp',b'Packaged model.anim is missing')]:
        enabled=features.get(flag,False)
        if source:assert (source in inputs)==enabled,(flag,inputs)
        assert (marker in binary)==enabled,flag
    asset=package/'assets/model.mesh'
    assert asset.is_file()==features.get('model_geometry',False)
    if asset.is_file():
        assert hashlib.sha256(asset.read_bytes()).hexdigest()==plan['model_asset']['mesh_sha256']
        assert not list(package.rglob('*.obj'))
        compiled=json.loads(subprocess.check_output([str(exe),'--describe'],cwd=package,timeout=15))
        assert compiled['model_asset']==plan['model_asset']
    animation=package/'assets/model.anim'
    assert animation.is_file()==features.get('model_animation',False)
    if animation.is_file():assert hashlib.sha256(animation.read_bytes()).hexdigest()==plan['model_asset']['animation']['sha256']
    rig=package/'assets/model.rig'
    assert rig.is_file()==features.get('ragdoll',False)
    if rig.is_file():assert hashlib.sha256(rig.read_bytes()).hexdigest()==plan['model_asset']['ragdoll']['sha256']
    assert 'ufbx' not in inputs.lower() and 'GltfLoader.cpp' not in inputs
    assert not any(p.suffix.lower() in ('.glb','.fbx') for p in package.rglob('*'))
    inventory=json.loads((package/'inventory.json').read_text())
    for name,info in inventory['files'].items():
        data=(package/name).read_bytes()
        assert len(data)==info['bytes'] and hashlib.sha256(data).hexdigest()==info['sha256'],name
    smoke_folder=ROOT/'output/validation/selected'/package.name.removeprefix('selected-')
    smoke=json.loads((smoke_folder/'result.json').read_text())
    log=(smoke_folder/'smoke.log').read_text(encoding='utf-8')
    assert smoke['exit_code']==0 and 'SELECTED EXIT frames=8' in log,log
    assert 'SIMULATION CHECK OK' in log,log
    assert ('Renderer initialized' in log)==features['render'],log
    assert ('XAudio2 / BGM' in log)==features['audio'],log
    return {'dynamic_audio_loader':dynamic_audio, 'game':config['game'],'features':features,'imports':dlls,
            'exe_bytes':exe.stat().st_size,'package_bytes':sum(f.stat().st_size for f in package.rglob('*') if f.is_file()),
            'shaders':sorted(f.name for f in (package/'shaders').glob('*.dxil'))}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suffix',required=True,help='Unique package/evidence suffix for this validation run')
    parser.add_argument('--audit-only',action='store_true',help='Recheck existing packages and saved smoke results without rebuilding')
    args=parser.parse_args()
    from build_runtime import safe_name
    safe_name(args.suffix)
    dumps=list(Path('C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC').glob('*/bin/Hostx64/x64/dumpbin.exe'))
    if not dumps: raise RuntimeError('dumpbin.exe required for dependency verification')
    dumpbin=sorted(dumps)[-1]
    evidence=ROOT/'output/validation/selected'/args.suffix
    if args.audit_only:
        if not evidence.is_dir(): raise ValueError('No existing validation run')
        old=evidence/'initial-results.json'
        if not old.exists(): old.write_bytes((evidence/'results.json').read_bytes())
    else:
        evidence.mkdir(parents=True,exist_ok=False)
    jobs=[]
    for sample in sorted((ROOT/'tools/advisor/examples').glob('selected-*.json')):
        config=json.loads(sample.read_text(encoding='utf-8-sig'))
        if config.get('distribution')=='signed' or config.get('ray_acceleration')=='hardware_required':
            print('Separate validation required (keys / DXR-capable GPU): '+sample.name,flush=True)
            continue
        jobs.append((sample.stem.removeprefix('selected-'),sample))
    extras={
        'pbr-no-shadow': {'game':'tps','material':'pbr','audio':'none'},
        'lit-shadow': {'game':'chase','shadows':'shadow_map','physics_hz':120},
        'headless-collision': {'game':'new_game','rendering':'none','material':'unlit','lights':'none','indirect_light':'none','physics':['collision'],'audio':'basic','physics_hz':1},
    }
    for name,config in extras.items():
        path=evidence/(name+'.json');path.write_text(json.dumps(config),encoding='utf-8');jobs.append((name,path))
    def work(job):
        name,config=job;name+='-'+args.suffix
        if not args.audit_only:
            with (evidence/(name+'.log')).open('w',encoding='utf-8') as log:
                process=subprocess.run([sys.executable,'-B',str(ROOT/'tools/advisor/build_runtime.py'),
                    '--config',str(config),'--name',name,'--smoke-test'],stdout=log,stderr=subprocess.STDOUT,timeout=600)
            if process.returncode: return {'name':name,'success':False,'log':str(evidence/(name+'.log'))}
        try:
            row=verify_package(ROOT/'output/packages'/('selected-'+name),dumpbin)
            return {'name':name,'success':True,**row}
        except Exception as exc:
            return {'name':name,'success':False,'error':str(exc)}
    rows=[]
    with ThreadPoolExecutor(max_workers=2) as pool:
        for future in as_completed([pool.submit(work,job) for job in jobs]):
            try: row=future.result()
            except Exception as exc: row={'success':False,'error':str(exc)}
            rows.append(row);print(json.dumps(row,ensure_ascii=True),flush=True)
            (evidence/'results.json').write_text(json.dumps({'success':len(rows)==len(jobs) and all(r['success'] for r in rows),'complete':len(rows)==len(jobs),'cases':rows},indent=2)+'\n',encoding='utf-8')
    return 0 if all(row['success'] for row in rows) else 1

if __name__=='__main__':raise SystemExit(main())
