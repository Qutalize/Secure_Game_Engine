"""Build-time, bounded GLB 2.0 geometry and linear blend skinning cooker.
Matrices are column-major glTF matrices (identical bytes to transposed DirectX rows).
No external buffers/images are opened. Materials are deliberately uniform at runtime.
"""
import bisect
import json
import math
import struct

IDENTITY=[1.,0,0,0,0,1.,0,0,0,0,1.,0,0,0,0,1.]
MAX_PALETTES=256
MAX_FRAMES=3601

def mul(a,b):
    return [sum(a[k*4+row]*b[col*4+k] for k in range(4)) for col in range(4) for row in range(4)]

def point(m,p):
    return tuple(sum(m[k*4+row]*p[k] for k in range(3))+m[12+row] for row in range(3))

def finite(values, count=None):
    if not isinstance(values,(list,tuple)) or (count is not None and len(values)!=count) or any(type(x) not in (float,int) or not math.isfinite(x) for x in values):
        raise ValueError('Expected finite numeric model values')
    return list(values)

def quaternion(q):
    q=finite(q,4);length=math.sqrt(sum(x*x for x in q))
    if length<1e-8:raise ValueError('Zero rotation quaternion')
    return [x/length for x in q]

def trs(t,q,s):
    t=finite(t,3);s=finite(s,3);x,y,z,w=quaternion(q)
    m=[1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w),0,
       2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w),0,
       2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y),0,*t,1]
    for col in range(3):
        for row in range(3):m[col*4+row]*=s[col]
    return m

def index(seq,i):
    if type(i)!=int or not 0<=i<len(seq):raise ValueError('Model index out of range')
    return seq[i]

class Glb:
    def __init__(self,raw):
        if len(raw)<20 or struct.unpack_from('<4sII',raw)!=(b'glTF',2,len(raw)):
            raise ValueError('Invalid GLB 2.0 header/length')
        chunks=[];offset=12
        while offset<len(raw):
            if offset+8>len(raw):raise ValueError('Truncated GLB chunk')
            size,kind=struct.unpack_from('<II',raw,offset);offset+=8
            if size%4 or offset+size>len(raw):raise ValueError('Invalid GLB chunk length')
            chunks.append((kind,raw[offset:offset+size]));offset+=size
        if len(chunks)!=2 or chunks[0][0]!=0x4e4f534a or chunks[1][0]!=0x004e4942:
            raise ValueError('GLB requires one JSON and one BIN chunk')
        try:self.doc=json.loads(chunks[0][1])
        except (ValueError,UnicodeError) as exc:raise ValueError('Invalid GLB JSON') from exc
        d=self.doc
        if d.get('asset',{}).get('version')!='2.0' or d.get('extensionsRequired'):
            raise ValueError('GLB extensionsRequired/version unsupported; export core glTF 2.0')
        buffers=d.get('buffers',[])
        if len(buffers)!=1 or 'uri' in buffers[0]:raise ValueError('GLB external buffers are not supported')
        size=buffers[0].get('byteLength')
        if type(size)!=int or size<0 or not size<=len(chunks[1][1])<=size+3:raise ValueError('Invalid GLB buffer length')
        self.bin=chunks[1][1][:size]

    def accessor(self,i,kind,components):
        a=index(self.doc.get('accessors',[]),i)
        if a.get('type')!=kind or a.get('componentType') not in components or 'sparse' in a:
            raise ValueError('Unsupported GLB accessor type/sparse data')
        ct=a['componentType'];fmt,width={5121:('B',1),5123:('H',2),5125:('I',4),5126:('f',4)}[ct]
        n={'SCALAR':1,'VEC3':3,'VEC4':4,'MAT4':16}[kind];count=a.get('count')
        if type(count)!=int or not 1<=count<=196608:raise ValueError('Invalid GLB accessor count')
        v=index(self.doc.get('bufferViews',[]),a.get('bufferView'))
        if v.get('buffer')!=0:raise ValueError('Invalid GLB buffer')
        start=v.get('byteOffset',0);length=v.get('byteLength');local=a.get('byteOffset',0);stride=v.get('byteStride',n*width)
        if any(type(x)!=int or x<0 for x in (start,length,local,stride)) or stride<n*width or ('byteStride' in v and (stride>252 or stride%4)) or start+length>len(self.bin) or local+(count-1)*stride+n*width>length or (start+local)%width:
            raise ValueError('GLB accessor outside its buffer view/alignment')
        normalized=a.get('normalized',False)
        if normalized and ct not in (5121,5123):raise ValueError('Invalid normalized GLB accessor')
        out=[list(struct.unpack_from('<'+fmt*n,self.bin,start+local+j*stride)) for j in range(count)]
        if normalized:
            maximum=(1<<(width*8))-1;out=[[x/maximum for x in row] for row in out]
        if ct==5126 and any(not math.isfinite(x) for row in out for x in row):raise ValueError('Non-finite GLB accessor')
        return out


def sample(times,values,t,mode,rotation):
    i=max(0,min(len(times)-1,bisect.bisect_right(times,t)-1));j=min(i+1,len(times)-1)
    at=lambda k:values[k*3+1] if mode=='CUBICSPLINE' else values[k]
    if i==j or mode=='STEP' or t<=times[0]:out=at(i)
    else:
        dt=times[j]-times[i];u=max(0,min(1,(t-times[i])/dt));a=at(i);b=at(j)
        if mode=='CUBICSPLINE':
            out=[(2*u**3-3*u*u+1)*a[k]+(u**3-2*u*u+u)*dt*values[i*3+2][k]+(-2*u**3+3*u*u)*b[k]+(u**3-u*u)*dt*values[j*3][k] for k in range(len(a))]
        elif rotation:
            a=quaternion(a);b=quaternion(b);dot=sum(x*y for x,y in zip(a,b))
            if dot<0:b=[-x for x in b];dot=-dot
            if dot>.9995:out=[x+(y-x)*u for x,y in zip(a,b)]
            else:
                angle=math.acos(max(-1,min(1,dot)));out=[(x*math.sin((1-u)*angle)+y*math.sin(u*angle))/math.sin(angle) for x,y in zip(a,b)]
        else:out=[x+(y-x)*u for x,y in zip(a,b)]
    return quaternion(out) if rotation else out


def load(raw,animate=False,clip_name=''):
    g=Glb(raw);d=g.doc;nodes=d.get('nodes',[])
    if not 1<=len(nodes)<=512:raise ValueError('GLB supports 1..512 nodes')
    parents=[-1]*len(nodes)
    for i,n in enumerate(nodes):
        for child in n.get('children',[]):
            index(nodes,child)
            if parents[child]!=-1:raise ValueError('GLB node has multiple parents')
            parents[child]=i
    def globals_at(channels=(),time=0):
        values=[{k:n.get(k,default) for k,default in [('translation',[0,0,0]),('rotation',[0,0,0,1]),('scale',[1,1,1])]} for n in nodes]
        for node,path,times,vs,mode in channels:values[node][path]=sample(times,vs,time,mode,path=='rotation')
        result=[None]*len(nodes);visiting=set()
        def visit(i,depth=0):
            if depth>128 or i in visiting:raise ValueError('GLB hierarchy cycle/depth limit')
            if result[i] is not None:return result[i]
            visiting.add(i);n=nodes[i];v=values[i]
            if 'matrix' in n:
                if any(k in n for k in ('translation','rotation','scale')):raise ValueError('GLB node matrix/TRS conflict')
                local=finite(n['matrix'],16)
                if any(abs(local[k])>1e-6 for k in (3,7,11)) or abs(local[15]-1)>1e-6:raise ValueError('GLB matrix must be affine')
            else:local=trs(v['translation'],v['rotation'],v['scale'])
            result[i]=mul(visit(parents[i],depth+1),local) if parents[i]>=0 else local
            visiting.remove(i);return result[i]
        for i in range(len(nodes)):visit(i)
        return result
    rest=globals_at()
    scene=index(d.get('scenes',[]),d.get('scene',0));active=set()
    def activate(i):
        index(nodes,i)
        if i in active:return
        active.add(i)
        for child in nodes[i].get('children',[]):activate(child)
    for i in scene.get('nodes',[]):activate(i)
    bindings=[];base=[];indices=[];joints=[];weights=[];has_skin=False
    for ni in sorted(active):
        n=nodes[ni]
        if 'mesh' not in n:continue
        mesh=index(d.get('meshes',[]),n['mesh']);offset=len(bindings)
        if 'skin' in n:
            has_skin=True;skin=index(d.get('skins',[]),n['skin']);js=skin.get('joints',[])
            if not js or len(js)!=len(set(js)):raise ValueError('Invalid GLB skin joint list')
            ibm=g.accessor(skin['inverseBindMatrices'],'MAT4',(5126,)) if 'inverseBindMatrices' in skin else [IDENTITY]*len(js)
            if len(ibm)!=len(js):raise ValueError('Inverse bind count mismatch')
            for node,m in zip(js,ibm):
                index(nodes,node)
                if any(abs(m[k])>1e-6 for k in (3,7,11)) or abs(m[15]-1)>1e-6:raise ValueError('Inverse bind must be affine')
                bindings.append((node,m))
        else:bindings.append((ni,IDENTITY));js=[ni]
        if len(bindings)>MAX_PALETTES:raise ValueError('Model exceeds 256 palette entries')
        for prim in mesh.get('primitives',[]):
            if prim.get('mode',4)!=4 or prim.get('targets') or prim.get('extensions'):raise ValueError('Export plain triangles without morph/compression extensions')
            attrs=prim.get('attributes',{});vs=g.accessor(attrs.get('POSITION'),'VEC3',(5126,));start=len(base)
            if 'skin' in n:
                if any(k.startswith(('JOINTS_','WEIGHTS_')) and k not in ('JOINTS_0','WEIGHTS_0') for k in attrs):raise ValueError('At most four skin influences are supported')
                jj=g.accessor(attrs.get('JOINTS_0'),'VEC4',(5121,5123));ww=g.accessor(attrs.get('WEIGHTS_0'),'VEC4',(5121,5123,5126))
                if len(jj)!=len(vs) or len(ww)!=len(vs):raise ValueError('Skin attribute count mismatch')
                for j,w in zip(jj,ww):
                    if any(type(x)!=int or not 0<=x<len(js) for x in j) or min(w)<0 or sum(w)<1e-8:raise ValueError('Invalid skin influence')
                    joints.append([x+offset for x in j]);weights.append([x/sum(w) for x in w])
            else:
                if 'JOINTS_0' in attrs or 'WEIGHTS_0' in attrs:raise ValueError('Skin attributes without a skin')
                joints.extend([[offset]*4 for _ in vs]);weights.extend([[1.,0,0,0] for _ in vs])
            ii=[x[0] for x in g.accessor(prim['indices'],'SCALAR',(5121,5123,5125))] if 'indices' in prim else list(range(len(vs)))
            if len(ii)%3 or any(type(x)!=int or not 0<=x<len(vs) for x in ii):raise ValueError('Invalid triangle indices')
            base.extend(vs);indices.extend(start+x for x in ii)
            if len(base)>65536 or len(indices)>196608:raise ValueError('Model geometry limit exceeded')
    if not indices:raise ValueError('GLB selected scene contains no triangles')
    animations=d.get('animations',[]);names=[a.get('name',str(i)) for i,a in enumerate(animations)]
    channels=[];duration=0.;name=''
    if animate:
        if not has_skin or not animations:raise ValueError('Skeletal playback requires a skin and an animation clip')
        if clip_name and names.count(clip_name)!=1:raise ValueError('Animation clip name is missing or ambiguous')
        ci=names.index(clip_name) if clip_name else 0;name=names[ci];animation=animations[ci];seen=set()
        for c in animation.get('channels',[]):
            target=c.get('target',{});ni=target.get('node');index(nodes,ni);path=target.get('path')
            if path not in ('translation','rotation','scale') or 'matrix' in nodes[ni] or (ni,path) in seen:raise ValueError('Unsupported/duplicate animation target')
            seen.add((ni,path));s=index(animation.get('samplers',[]),c.get('sampler'));mode=s.get('interpolation','LINEAR')
            if mode not in ('LINEAR','STEP','CUBICSPLINE'):raise ValueError('Unsupported animation interpolation')
            ts=[v[0] for v in g.accessor(s.get('input'),'SCALAR',(5126,))]
            if ts[0]<0 or any(a>=b for a,b in zip(ts,ts[1:])):raise ValueError('Animation times must strictly increase')
            vs=g.accessor(s.get('output'),'VEC4' if path=='rotation' else 'VEC3',(5126,))
            if len(vs)!=len(ts)*(3 if mode=='CUBICSPLINE' else 1):raise ValueError('Animation key count mismatch')
            channels.append((ni,path,ts,vs,mode));duration=max(duration,ts[-1])
        if not 0<duration<=60:raise ValueError('Animation duration must be 0..60 seconds')
    frame_count=math.ceil(duration*60)+1 if animate else 1
    if frame_count>MAX_FRAMES:raise ValueError('Too many animation samples')
    def palette(globals_):return [mul(globals_[node],ibm) for node,ibm in bindings]
    frames=[palette(globals_at(channels,duration*f/(frame_count-1))) for f in range(frame_count)] if animate else [palette(rest)]
    positions=[]
    for p,j,w in zip(base,joints,weights):
        positions.append([sum(point(frames[0][j[k]],p)[axis]*w[k] for k in range(4)) for axis in range(3)])
    initial_globals=globals_at(channels,0) if animate else rest
    binding_nodes=[node for node,_ in bindings];rig=[]
    for node in binding_nodes:
        parent=parents[node]
        while parent>=0 and parent not in binding_nodes:parent=parents[parent]
        rig.append({'parent':binding_nodes.index(parent) if parent>=0 else -1,
                    'anchor':point(initial_globals[node],[0,0,0]),'node':node})
    return dict(rig=rig,positions=positions,indices=indices,base=base,joints=joints,weights=weights,frames=frames,duration=duration,clip=name,clips=names)
