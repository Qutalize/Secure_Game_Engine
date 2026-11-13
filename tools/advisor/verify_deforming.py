"""Audit optional deforming collision packages and the self-authored two-bone fixture."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
from build_runtime import ROOT,under_root
from verify_advanced import audit

def verify(headless,signed,disabled,static,probe,ray_probe):
    evidence=under_root(ROOT/'output/validation/selected/deforming');evidence.mkdir(parents=True,exist_ok=True)
    packages=[Path(x).resolve() for x in (headless,signed,disabled,static)]
    headless,signed,disabled,static=packages
    audits=[]
    for package in packages:
        result=audit(package,evidence/package.name);audits.append(result)
        if result['audit']['features'].get('model_deforming'):
            log=(ROOT/'output/validation/selected'/package.name.removeprefix('selected-')/'smoke.log').read_text(encoding='utf-8')
            assert 'DEFORMING SCENE CHECK OK' in log,'Rebuild the latest scene self-test'
    assert audits[0]['audit']['features']['model_deforming'] and not audits[0]['audit']['features']['render']
    assert audits[1]['audit']['features']['model_deforming'] and audits[1]['audit']['features']['distribution_signature']
    assert not audits[2]['audit']['features']['model_deforming'] and not audits[2]['audit']['features']['rigid_body']
    assert audits[3]['audit']['features']['model_static'] and not audits[3]['audit']['features']['model_deforming']
    native=[]
    for package in (headless,signed):
        result=subprocess.run([str(probe),str(package/'assets/model.mesh'),str(package/'assets/model.anim')],cwd=ROOT.parent,capture_output=True,timeout=45)
        log=result.stdout+result.stderr;(evidence/('native-'+package.name+'.log')).write_bytes(log)
        match=re.search(rb'DEFORMING CHECK OK checks=(\d+)',log)
        assert result.returncode==0 and match,log
        native.append({'package':package.name,'checks':int(match[1]),'exit_code':result.returncode})
    result=subprocess.run([str(ray_probe)],cwd=ROOT.parent,capture_output=True,timeout=60)
    log=result.stdout+result.stderr;(evidence/'gpu-rays.log').write_bytes(log)
    assert result.returncode==0 and b'GPU RAY CHECK OK' in log,log
    frames=len(re.findall(rb'frame=\d+ slot=',log));assert frames==6
    repeated=[]
    for i in range(3):
        metrics=evidence/f'repeat-{i+1}.csv'
        result=subprocess.run([str(signed/'SelectedGame.exe'),'--smoke-test','--frames','180','--metrics',str(metrics)],cwd=ROOT.parent,capture_output=True,timeout=60)
        log=result.stdout+result.stderr;(evidence/f'repeat-{i+1}.log').write_bytes(log)
        assert result.returncode==0 and b'SELECTED EXIT frames=180' in log,log
        rows=list(csv.DictReader(metrics.open(encoding='utf-8')))
        intervals=[float(row['value_ms']) for row in rows if row['kind']=='frame']
        repeated.append({'frames':180,'exit_code':0,'cpu_update_interval_median_ms':statistics.median(intervals),
                         'cpu_update_interval_max_ms':max(intervals)})
    sources=['engine/physics/DeformingMeshCollider.h','engine/physics/DeformingMeshCollider.cpp',
             'engine/physics/RigidBodyWorld.h','engine/physics/RigidBodyWorld.cpp','game/creator/Main.cpp',
             'engine/core/EngineFeatures.h','cmake/SelectedRuntime.cmake','tools/advisor/runtime_plan.py',
             'tools/advisor/advisor.py','tools/advisor/catalog.py','tools/advisor/model_asset.py','tools/advisor/test_deforming.py',
             'tools/advisor/verify_runtime.py','tools/advisor/verify_deforming.py','tools/deforming_test/main.cpp']
    raw=(evidence/'unit-tests.log').read_bytes();unit=raw.decode('utf-16' if raw.startswith(b'\xff\xfe') else 'utf-8-sig');match=re.search(r'Ran (\d+) tests',unit);assert match and unit.rstrip().endswith('OK')
    summary={'date':'2026-10-29','success':True,'python_tests':int(match[1]),'native':native,
             'audited_packages':[{'package':a['package'],**a['audit']} for a in audits],
             'signature_cases':len(audits[1]['cases']),'gpu_ray_checks':frames*16,'repeat':repeated,
             'source_sha256':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sources},
             'limitations':['Static shape replacement controlled by animation; no ragdoll or force feedback to bones',
                            'Deformation is discrete; rapidly moving triangles can tunnel; physical contacts follow winding',
                            '4096 triangles maximum, per-substep rebuild; large-model performance not measured',
                            'No positive DXR GPU run, separate-PC or human visual/controller evaluation',
                            'CPU update intervals include rendering/VSync and self-test conditions; not display or input latency']}
    encoded=json.dumps(summary,ensure_ascii=False,indent=2)+'\n'
    (evidence/'summary.json').write_text(encoded,encoding='utf-8')
    (ROOT/'docs/maintenance/deforming-collision-20261029.json').write_text(encoded,encoding='utf-8')
    return summary

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('headless','signed','disabled','static','probe','ray-probe'):parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args();summary=verify(args.headless,args.signed,args.disabled,args.static,args.probe,args.ray_probe)
    print('PASS:',summary['python_tests'],'Python tests;',sum(r['checks'] for r in summary['native']),'native checks;',summary['signature_cases'],'signature cases; GPU',summary['gpu_ray_checks'])
