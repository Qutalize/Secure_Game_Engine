"""Verify optional ragdoll/continuous-deformation builds using the self-authored two-bone fixtures."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import struct
import subprocess
import sys
from build_runtime import ROOT,under_root
from verify_advanced import audit,disposable

def verify(packages,probe,ray_probe,deforming_probe):
    evidence=under_root(ROOT/'output/validation/selected/dynamics');evidence.mkdir(parents=True,exist_ok=True)
    packages=[Path(p).resolve() for p in packages]
    records=[audit(p,evidence/p.name) for p in packages]
    matrix={(a['audit']['features']['ragdoll'],a['audit']['features']['deformation_ccd']) for a in records}
    assert matrix=={(False,False),(True,False),(False,True),(True,True)},matrix
    full=next(p for p in packages if (p/'distribution.sig').is_file())
    glb=next(p for p in packages if not (p/'distribution.sig').is_file() and json.loads((p/'runtime-plan.json').read_text(encoding='utf-8'))['features']['ragdoll'] and json.loads((p/'runtime-plan.json').read_text(encoding='utf-8'))['features']['deformation_ccd'])
    checks=[]
    for p in (glb,full):
        result=subprocess.run([str(probe),str(p/'assets/model.mesh'),str(p/'assets/model.anim'),str(p/'assets/model.rig')],cwd=ROOT.parent,capture_output=True,timeout=45)
        log=result.stdout+result.stderr;(evidence/('native-'+p.name+'.log')).write_bytes(log)
        match=re.search(rb'DYNAMICS CHECK OK checks=(\d+)',log);assert result.returncode==0 and match,log
        checks.append({'package':p.name,'checks':int(match[1]),'success':True})
    # The earlier deforming-mesh behavior must still pass with the new library compiled in.
    result=subprocess.run([str(deforming_probe),str(glb/'assets/model.mesh'),str(glb/'assets/model.anim')],cwd=ROOT.parent,capture_output=True,timeout=45)
    log=result.stdout+result.stderr;(evidence/'deforming-regression.log').write_bytes(log)
    regression=re.search(rb'DEFORMING CHECK OK checks=(\d+)',log);assert result.returncode==0 and regression,log
    long_run=subprocess.run([str(glb/'SelectedGame.exe'),'--smoke-test','--frames','1200'],cwd=ROOT.parent,capture_output=True,timeout=45)
    long_log=long_run.stdout+long_run.stderr;(evidence/'headless-1200.log').write_bytes(long_log)
    assert long_run.returncode==0 and b'SELECTED EXIT frames=1200' in long_log,long_log
    original=(glb/'assets/model.rig').read_bytes()
    def change(offset,fmt,value):
        data=bytearray(original);struct.pack_into(fmt,data,offset,value);return bytes(data)
    invalid={'missing':None,'magic':b'badmagic'+original[8:],'truncated':original[:-1],'trailing':original+b'x',
             'count':change(8,'<I',33),'parent-range':change(12,'<i',99),'cycle':change(12,'<i',1),
             'size-nan':change(28,'<f',float('nan')),'size-zero':change(28,'<f',0),'size-cap':change(28,'<f',21),
             'anchor-inf':change(40,'<f',float('inf')),'center-nan':change(16,'<f',float('nan'))}
    rejected=[]
    with disposable(evidence) as tmp:
        copy=tmp/'package';shutil.copytree(glb,copy);asset=copy/'assets/model.rig'
        for name,data in invalid.items():
            if data is None:asset.unlink()
            else:asset.write_bytes(data)
            result=subprocess.run([str(copy/'SelectedGame.exe'),'--self-test'],cwd=ROOT.parent,capture_output=True,timeout=45)
            log=result.stdout+result.stderr;(evidence/('rig-reject-'+name+'.log')).write_bytes(log)
            assert result.returncode==2 and b'SELECTED START' not in log,(name,log)
            rejected.append(name)
    assert (glb/'assets/model.rig').read_bytes()==original
    result=subprocess.run([str(ray_probe)],cwd=ROOT.parent,capture_output=True,timeout=60)
    log=result.stdout+result.stderr;(evidence/'gpu-rays.log').write_bytes(log)
    assert result.returncode==0 and b'GPU RAY CHECK OK' in log,log
    frames=len(re.findall(rb'frame=\d+ slot=',log));assert frames==6
    # RunMetrics intentionally stops a timed sample after 5 seconds. Keep that measurement
    # separate from a frame-count endurance run, so a slow device still must finish all frames.
    metrics=evidence/'timed-sample.csv'
    result=subprocess.run([str(full/'SelectedGame.exe'),'--smoke-test','--frames','180','--metrics',str(metrics)],cwd=ROOT.parent,capture_output=True,timeout=60)
    log=result.stdout+result.stderr;(evidence/'timed-sample.log').write_bytes(log)
    completed=re.search(rb'SELECTED EXIT frames=(\d+)',log);assert result.returncode==0 and completed,log
    completed=int(completed[1]);assert 1<=completed<=180
    rows=list(csv.DictReader(metrics.open(encoding='utf-8')));values=[float(x['value_ms']) for x in rows if x['kind']=='frame']
    timed={'requested_frames':180,'completed_frames':completed,'sample_limit_seconds':5,'exit_code':0,
           'cpu_update_median_ms':statistics.median(values),'cpu_update_max_ms':max(values)}
    repeats=[]
    for i in range(3):
        result=subprocess.run([str(full/'SelectedGame.exe'),'--smoke-test','--frames','180'],cwd=ROOT.parent,capture_output=True,timeout=60)
        log=result.stdout+result.stderr;(evidence/f'repeat-{i+1}.log').write_bytes(log)
        assert result.returncode==0 and b'SELECTED EXIT frames=180' in log,log
        repeats.append({'frames':180,'success':True,'time_limit_enabled':False})
    result=subprocess.run([sys.executable,'-m','unittest','discover','-s',str(ROOT/'tools/advisor'),'-p','test_*.py','-v'],capture_output=True)
    log=result.stdout+result.stderr;(evidence/'unit-tests.log').write_bytes(log);assert result.returncode==0,log
    unit=int(re.search(rb'Ran (\d+) tests',log)[1])
    sources=['engine/physics/RagdollModel.h','engine/physics/RagdollModel.cpp','engine/physics/ContinuousDeformation.h','engine/physics/ContinuousDeformation.cpp',
             'engine/physics/RigidBodyWorld.h','engine/physics/RigidBodyWorld.cpp','engine/geometry/SkeletalAnimation.h','engine/geometry/SkeletalAnimation.cpp',
             'engine/core/EngineFeatures.h','game/creator/Main.cpp','cmake/SelectedRuntime.cmake','tools/advisor/advisor.py','tools/advisor/runtime_plan.py',
             'tools/advisor/model_asset.py','tools/advisor/ragdoll_asset.py','tools/advisor/gltf_asset.py','tools/advisor/build_runtime.py',
             'tools/advisor/verify_runtime.py','tools/advisor/verify_advanced.py','tools/advisor/verify_dynamics.py','tools/advisor/test_dynamics.py','tools/model_cooker/main.cpp',
             'tools/dynamics_test/main.cpp','tools/ray_test/main.cpp']
    summary={'date':'2026-10-29','success':True,'python_tests':unit,'native':checks,'deforming_regression_checks':int(regression[1]),'headless_endurance_frames':1200,
             'malformed_rig_cases':rejected,'gpu_ray_checks':frames*16,'gpu_independently_updated_meshes':2,'repeats':repeats,'timed_sample':timed,
             'audits':[{'package':a['package'],**a['audit']} for a in records],
             'signature_cases':sum(len(a['cases']) for a in records),'source_sha256':{p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in sources},
             'limitations':['Ragdoll uses automatic box proxies, generic joint axes/limits, no self-collision, motors, animation blending or recovery',
                            'Continuous deformation stops at contact with enclosing spheres; may stop early, does not impart moving-surface velocity',
                            'CCD trajectory is piecewise-linear baked vertex motion; endpoint root rotation changes rejected',
                            'Dynamic body trajectory prediction includes velocity/gravity; arbitrary coupled collision/constraint trajectories are not globally solved by this deformation limiter',
                            'Ragdoll bodies use Jolt LinearCast; no exact continuous triangle-triangle collision for the ragdoll skin',
                            'No separate PC, human visual/input evaluation, large model performance or DXR hardware success test']}
    data=json.dumps(summary,ensure_ascii=False,indent=2)+'\n';(evidence/'summary.json').write_text(data,encoding='utf-8')
    (ROOT/'docs/maintenance/ragdoll-ccd-20261029.json').write_text(data,encoding='utf-8');return summary

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('packages',type=Path,nargs='+')
    for name in ('probe','ray-probe','deforming-probe'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();result=verify(args.packages,args.probe,args.ray_probe,args.deforming_probe)
    print('PASS:',result['python_tests'],'Python tests;',sum(x['checks'] for x in result['native']),'native checks;',result['signature_cases'],'signature cases;',result['gpu_ray_checks'],'GPU checks')
