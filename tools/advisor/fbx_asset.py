"""Pinned build-only ufbx tool, compiled on demand and never installed in player packages."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

def load(source,animate=False,clip_name=''):
    from build_runtime import find_cmake,under_root
    root=Path(__file__).resolve().parents[2]
    inputs=[root/'tools/model_cooker'/x for x in ('CMakeLists.txt','main.cpp')]+[root/'third_party/ufbx'/x for x in ('ufbx.c','ufbx.h')]
    digest=hashlib.sha256(b''.join(p.read_bytes() for p in inputs)).hexdigest()[:12]
    cache=under_root(root/'output/build-tools'/('fbx-'+digest));exe=cache/'Release/fbx_cooker.exe'
    if not exe.is_file():
        cache.mkdir(parents=True,exist_ok=True);cmake=find_cmake()
        with (cache/'build.log').open('w',encoding='utf-8') as log:
            for command in ([cmake,'-S',str(root/'tools/model_cooker'),'-B',str(cache),'-G','Visual Studio 17 2022','-A','x64'],[cmake,'--build',str(cache),'--config','Release','--parallel','2']):
                done=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,timeout=180)
                if done.returncode:raise ValueError('FBX tool build failed; see '+str(cache/'build.log'))
    with tempfile.TemporaryDirectory(prefix='convert-',dir=cache) as temp:
        output=Path(temp)/'model.json'
        done=subprocess.run([str(exe),str(source),str(output),'skeletal' if animate else 'static',clip_name],capture_output=True,timeout=120)
        if done.returncode:raise ValueError(done.stderr.decode('utf-8',errors='replace'))
        if output.stat().st_size>96*1024*1024:raise ValueError('FBX intermediate exceeds 96 MiB')
        return json.loads(output.read_text(encoding='utf-8'))
