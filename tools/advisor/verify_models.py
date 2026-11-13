"""Validate the packaged native mesh reader using disposable copies, without GPU initialization."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
from build_runtime import ROOT,under_root
from verify_advanced import disposable

def verify(package):
    package=package.resolve();config=json.loads((package/'request.json').read_text(encoding='utf-8'))
    if config['distribution']!='unsigned' or config['model']=='none':raise ValueError('Use an unsigned model package to test the reader itself')
    original=(package/'assets/model.mesh').read_bytes();vertices,triangles=struct.unpack_from('<II',original,8)
    def overwrite(offset,format,value):
        data=bytearray(original);struct.pack_into(format,data,offset,value);return bytes(data)
    cases={'magic':b'INVALID!'+original[8:],'header':original[:10],'vertex-cap':overwrite(8,'<I',0xffffffff),
           'triangle-cap':overwrite(12,'<I',0xffffffff),'zero-count':overwrite(12,'<I',0),'truncated':original[:-4],
           'trailing':original+b'X','nan':overwrite(16,'<f',float('nan')),'coordinate-limit':overwrite(16,'<f',1e9),
           'index':overwrite(16+vertices*12,'<I',vertices),'degenerate':overwrite(16+vertices*12+4,'<I',struct.unpack_from('<I',original,16+vertices*12)[0]),
           'missing':None}
    out=under_root(ROOT/'output/validation/selected/models/native-loader');out.mkdir(parents=True,exist_ok=True);rows=[]
    for name,data in cases.items():
        with disposable(out) as temp:
            copy=temp/'package';shutil.copytree(package,copy);asset=copy/'assets/model.mesh'
            if data is None:asset.unlink()
            else:asset.write_bytes(data)
            p=subprocess.run([str(copy/'SelectedGame.exe'),'--self-test'],cwd=ROOT.parent,capture_output=True,timeout=30)
            log=p.stdout+p.stderr;(out/(name+'.log')).write_bytes(log)
            ok=p.returncode==2 and b'SELECTED START' not in log
            rows.append({'case':name,'success':ok,'exit_code':p.returncode})
            if not ok:raise AssertionError((name,log))
    assert (package/'assets/model.mesh').read_bytes()==original
    (out/'results.json').write_text(json.dumps({'success':True,'cases':rows},indent=2)+'\n',encoding='utf-8')
    print('Native mesh reader: 12 malformed/missing asset cases rejected')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('package',type=Path);args=parser.parse_args();verify(args.package)
