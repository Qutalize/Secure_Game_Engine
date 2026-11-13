"""Bounded, geometry-only OBJ cooking. No material/texture paths are followed."""
import hashlib
import math
from pathlib import Path, PurePosixPath
import re
import struct

MAX_VERTICES=65536
MAX_TRIANGLES=65536
MAX_SOURCE_BYTES=8*1024*1024
MAGIC=b'GEMESH1\0'

def validate_path(value, model=None):
    if not isinstance(value,str) or not value or len(value)>240 or '\\' in value:
        raise ValueError('model_path must be a project-relative model path using /')
    p=PurePosixPath(value)
    if p.is_absolute() or any(x in ('..','.') for x in value.split('/')) or ':' in value or p.suffix.lower() not in ('.obj','.glb','.fbx') or (model and p.suffix.lower()!='.'+model):
        raise ValueError('model_path must stay inside the project and match the selected .obj/.glb/.fbx format')
    return p

def parse_obj(text, scale=1):
    vertices=[];indices=[]
    for line_number,line in enumerate(text.splitlines(),1):
        tokens=line.partition('#')[0].split()
        if not tokens:continue
        tag,*values=tokens
        if tag=='v':
            if len(values)!=3:raise ValueError(f'OBJ line {line_number}: v requires exactly x y z')
            try:
                xyz=struct.unpack('<3f',struct.pack('<3f',*(float(v)*scale for v in values)))
            except (ValueError,OverflowError,struct.error) as exc:raise ValueError(f'OBJ line {line_number}: invalid vertex') from exc
            if not all(math.isfinite(v) and abs(v)<=100000 for v in xyz):raise ValueError('Invalid model coordinates')
            vertices.append(xyz)
            if len(vertices)>MAX_VERTICES:raise ValueError('Model exceeds 65536 vertices')
        elif tag=='f':
            if len(values)!=3:raise ValueError(f'OBJ line {line_number}: export triangulated faces (exactly 3 vertices)')
            for value in values:
                if not re.fullmatch(r'-?[1-9][0-9]*(?:/(?:-?[1-9][0-9]*)?(?:/-?[1-9][0-9]*)?)?',value):
                    raise ValueError(f'OBJ line {line_number}: invalid face reference')
                raw=int(value.split('/')[0]);index=raw-1 if raw>0 else len(vertices)+raw
                if not 0<=index<len(vertices):raise ValueError(f'OBJ line {line_number}: vertex index out of range')
                indices.append(index)
            if len(indices)>3*MAX_TRIANGLES:raise ValueError('Model exceeds 65536 triangles')
        elif tag not in ('vn','vt','o','g','s','usemtl','mtllib'):
            raise ValueError(f'OBJ line {line_number}: unsupported directive {tag}')
    if len(vertices)<3 or not indices:raise ValueError('Model must contain vertices and triangles')
    for offset in range(0,len(indices),3):
        a,b,c=(vertices[indices[offset+j]] for j in range(3));u=[b[j]-a[j] for j in range(3)];v=[c[j]-a[j] for j in range(3)]
        n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
        if sum(x*x for x in n)<=1e-16:raise ValueError('Degenerate model triangle')
    # Unreferenced OBJ vertices must not enlarge the collision hull.
    used=sorted(set(indices));remap={old:new for new,old in enumerate(used)}
    return [vertices[i] for i in used],[remap[i] for i in indices]

def require_volume(vertices):
    if len(vertices)<4 or len(vertices)>4096:raise ValueError('Convex hull requires 4..4096 referenced points')
    a=vertices[0]
    sub=lambda p,q:tuple(p[i]-q[i] for i in range(3))
    dot=lambda p,q:sum(p[i]*q[i] for i in range(3))
    cross=lambda p,q:(p[1]*q[2]-p[2]*q[1],p[2]*q[0]-p[0]*q[2],p[0]*q[1]-p[1]*q[0])
    b=max((sub(v,a) for v in vertices),key=lambda v:dot(v,v))
    n=max((cross(b,sub(v,a)) for v in vertices),key=lambda v:dot(v,v))
    if max(abs(dot(n,sub(v,a))) for v in vertices)<=1e-6*max(dot(b,b)**1.5,1e-9):
        raise ValueError('Dynamic convex hull needs a non-coplanar volume')

def _geometry(vertices,indices,drop_degenerate=False):
    if not 3<=len(vertices)<=MAX_VERTICES or not indices or len(indices)%3 or len(indices)>3*MAX_TRIANGLES:
        raise ValueError('Invalid cooked model counts')
    if any(type(i)!=int or not 0<=i<len(vertices) for i in indices):raise ValueError('Invalid model indices')
    vertices=[struct.unpack('<3f',struct.pack('<3f',*p)) for p in vertices]
    if any(not math.isfinite(x) or abs(x)>100000 for p in vertices for x in p):raise ValueError('Invalid model coordinates')
    kept=[];discarded=0
    for i in range(0,len(indices),3):
        a,b,c=(vertices[j] for j in indices[i:i+3]);u=[b[j]-a[j] for j in range(3)];v=[c[j]-a[j] for j in range(3)]
        n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
        if sum(x*x for x in n)<=1e-16:
            if not drop_degenerate:raise ValueError('Degenerate animated triangle; clean the bind/first pose before export')
            discarded+=1
        else:kept.extend(indices[i:i+3])
    if not kept:raise ValueError('Model has no non-degenerate triangles')
    return vertices,kept,discarded


def _animation(asset,scale,used):
    frames=asset['frames'];joint_count=len(frames[0]);duration=asset['duration']
    if not 1<=joint_count<=256 or not 2<=len(frames)<=3601 or not 0<duration<=60:raise ValueError('Invalid animation limits')
    data=bytearray(b'GEANIM1\0'+struct.pack('<IIIf',len(used),joint_count,len(frames),duration))
    for i in used:
        base=asset['base'][i];joints=asset['joints'][i];weights=asset['weights'][i]
        if len(joints)!=4 or any(type(j)!=int or not 0<=j<joint_count for j in joints) or len(weights)!=4 or any(not math.isfinite(w) or w<0 for w in weights) or abs(sum(weights)-1)>1e-4:raise ValueError('Invalid cooked skin weights')
        if len(base)!=3 or any(not math.isfinite(v) or abs(v)>100000 for v in base):raise ValueError('Invalid skin bind positions')
        data+=struct.pack('<3f4H4f',*base,*joints,*weights)
    for frame in frames:
        if len(frame)!=joint_count:raise ValueError('Inconsistent palette count')
        for matrix in frame:
            if len(matrix)!=16 or any(not math.isfinite(v) or abs(v)>100000 for v in matrix):raise ValueError('Invalid skin matrix')
            if any(abs(matrix[i])>1e-6 for i in (3,7,11)) or abs(matrix[15]-1)>1e-6:raise ValueError('Skin matrix must be affine')
            data+=struct.pack('<16f',*(v*scale if i%4<3 else v for i,v in enumerate(matrix)))
    if len(data)>64*1024*1024:raise ValueError('Animation exceeds 64 MiB')
    return bytes(data)


def cook(config,root):
    if config['model']=='none':return None
    relative=validate_path(config['model_path'],config['model']);root=root.resolve();source=(root/relative).resolve()
    if not source.is_relative_to(root):raise ValueError('Model source escapes project via a link')
    if source.stat().st_size>MAX_SOURCE_BYTES:raise ValueError('Model source exceeds 8 MiB')
    raw=source.read_bytes()
    if len(raw)>MAX_SOURCE_BYTES:raise ValueError('Model source exceeds 8 MiB')
    animation=None;rig_data=None;asset=None;discarded=0;scale=config['model_scale'];animated=config['animation']=='skeletal'
    try:
        if config['model']=='obj':
            if animated:raise ValueError('OBJ has no skeleton')
            vertices,indices=parse_obj(raw.decode('utf-8-sig'),scale)
        else:
            if config['model']=='glb':
                from gltf_asset import load
                asset=load(raw,animated,config['animation_clip'])
            else:
                from fbx_asset import load
                asset=load(source,animated,config['animation_clip'])
                if source.read_bytes()!=raw:raise ValueError('FBX source changed during conversion')
            vertices=[[v*scale for v in p] for p in asset['positions']];indices=asset['indices']
            vertices,indices,discarded=_geometry(vertices,indices,drop_degenerate=not animated)
            used=sorted(set(indices));remap={old:new for new,old in enumerate(used)}
            vertices=[vertices[i] for i in used];indices=[remap[i] for i in indices]
            if animated:animation=_animation(asset,scale,used)
            if config.get('ragdoll')=='passive':
                from ragdoll_asset import cook as cook_rig
                rig_data=cook_rig(asset,scale,used)
        if config['model_physics']=='deforming_mesh':
            if not animated:raise ValueError('Deforming collision requires skeletal animation')
            if len(indices)//3>4096:raise ValueError('Deforming collision exceeds 4096 triangles')
        if config['model_physics']=='convex_hull':require_volume(vertices)
        data=MAGIC+struct.pack('<II',len(vertices),len(indices)//3)
        data+=b''.join(struct.pack('<3f',*p) for p in vertices)+struct.pack('<'+'I'*len(indices),*indices)
    except (KeyError,TypeError,IndexError,OverflowError,struct.error,UnicodeError) as exc:
        raise ValueError('Invalid or unsupported model data: '+str(exc)) from exc
    info={'source_sha256':hashlib.sha256(raw).hexdigest(),'mesh_sha256':hashlib.sha256(data).hexdigest(),
          'vertices':len(vertices),'triangles':len(indices)//3,'format':'GEMESH1','path':'assets/model.mesh',
          'source_format':config['model'],'materials':'uniform color; source materials, UVs and supplied normals are not imported'}
    if discarded:info['discarded_degenerate_triangles']=discarded
    if animation:
        info['animation']={'path':'assets/model.anim','sha256':hashlib.sha256(animation).hexdigest(),'format':'GEANIM1',
            'clip':asset['clip'],'available_clips':asset['clips'],'duration':asset['duration'],'samples':len(asset['frames']),
            'palette_entries':len(asset['frames'][0]),'sampling':'60 Hz or finer; matrix interpolation; four-weight CPU linear blend skinning'}
    if rig_data:info['ragdoll']={'path':'assets/model.rig','sha256':hashlib.sha256(rig_data).hexdigest(),'bones':len(asset['frames'][0]),'format':'GERIG1','proxy':'initial-pose axis-aligned boxes; limited swing/twist joints; no self-collision'}
    return {'data':data,'animation_data':animation,'rig_data':rig_data,'info':info}
