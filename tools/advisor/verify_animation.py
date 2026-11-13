"""Verify CC0 two-bone fixtures, native poses and bounded packaged animation loading."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
from build_runtime import ROOT,under_root
from verify_advanced import disposable


def poses(package,probe,evidence):
    rows=[]
    for time,loop in [(0,False),(.25,False),(.5,False),(1,False),(2,False),(3,False),(3,True)]:
        p=subprocess.run([str(probe),str(package/'assets/model.mesh'),str(package/'assets/model.anim'),str(time),str(int(loop))],capture_output=True,timeout=20)
        (evidence/f'pose-{time}-{loop}.log').write_bytes(p.stdout+p.stderr);assert p.returncode==0,p.stderr
        pose=json.loads(next(line[5:] for line in p.stdout.decode().splitlines() if line.startswith('POSE ')))
        t=time%2 if loop else min(time,2);amplitude=t if t<=1 else 2-t;angle=amplitude*math.pi/4
        assert len(pose)==6,'This verification uses the self-authored two-bone fixture'
        for i,(x,y) in enumerate([(-.4,0),(.4,0),(-.4,1),(.4,1),(-.4,2),(.4,2)]):
            weight=[0,0,.5,.5,1,1][i];rx=x*math.cos(angle)-(y-1)*math.sin(angle);ry=1+x*math.sin(angle)+(y-1)*math.cos(angle)
            expected=[amplitude+x*(1-weight)+rx*weight,y*(1-weight)+ry*weight,0]
            assert max(abs(a-b) for a,b in zip(pose[i],expected))<1e-5,(time,i,pose[i],expected)
        rows.append({'time':time,'loop':loop,'success':True})
    return rows


def verify(package,probe):
    package=package.resolve();evidence=under_root(ROOT/'output/validation/selected/animation'/package.name);evidence.mkdir(parents=True,exist_ok=True)
    plan=json.loads((package/'runtime-plan.json').read_text(encoding='utf-8'))
    assert not plan['features']['distribution_signature'],'Use an unsigned copy for parser tests; signed startup has separate tamper tests'
    before={p.relative_to(package).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in package.rglob('*') if p.is_file()}
    rows=poses(package,probe,evidence);original=(package/'assets/model.anim').read_bytes()
    def changed(offset,fmt,value):
        data=bytearray(original);struct.pack_into(fmt,data,offset,value);return bytes(data)
    vertices,joints,frames=struct.unpack_from('<III',original,8);matrix=24+vertices*36
    cases={'missing':None,'magic':b'badmagic'+original[8:],'truncated':original[:-1],'trailing':original+b'X',
        'vertices':changed(8,'<I',65537),'joints-zero':changed(12,'<I',0),'joints-cap':changed(12,'<I',257),
        'frames-zero':changed(16,'<I',0),'frames-cap':changed(16,'<I',3602),'duration-zero':changed(20,'<f',0),
        'duration-nan':changed(20,'<f',float('nan')),'bind-nan':changed(24,'<f',float('nan')),
        'bind-cap':changed(24,'<f',100001),'joint-range':changed(36,'<H',joints),
        'weight-nan':changed(44,'<f',float('nan')),'weight-negative':changed(44,'<f',-1),'weight-sum':changed(44,'<f',.25),
        'matrix-nan':changed(matrix,'<f',float('nan')),'matrix-affine':changed(matrix+12,'<f',1),
        'wrong-pose':changed(matrix+48,'<f',20)}
    failures=[]
    with disposable(evidence) as temp:
        copy=temp/'package';shutil.copytree(package,copy);asset=copy/'assets/model.anim'
        for name,data in cases.items():
            if data is None:asset.unlink()
            else:asset.write_bytes(data)
            result=subprocess.run([str(copy/'SelectedGame.exe'),'--self-test'],cwd=ROOT.parent,capture_output=True,timeout=20)
            log=result.stdout+result.stderr;(evidence/(name+'.log')).write_bytes(log)
            assert result.returncode==2 and b'SELECTED START' not in log,(name,result.returncode,log)
            failures.append({'case':name,'exit_code':result.returncode,'success':True})
    after={p.relative_to(package).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in package.rglob('*') if p.is_file()}
    assert before==after
    result={'package':str(package),'poses':rows,'malformed':failures,'success':True}
    (evidence/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');return result

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('package',type=Path);parser.add_argument('--probe',type=Path,required=True);args=parser.parse_args()
    result=verify(args.package,args.probe);print('Native poses:',len(result['poses']),'malformed assets:',len(result['malformed']))
