"""Self-authored CC0 two-bone ribbon; no third-party character/animation assets."""
import json
import math
from pathlib import Path
import struct

POSITIONS=[[-.4,0,0],[.4,0,0],[-.4,1,0],[.4,1,0],[-.4,2,0],[.4,2,0]]
INDICES=[0,2,1,1,2,3,2,4,3,3,4,5]
IDENTITY=[1.,0,0,0,0,1.,0,0,0,0,1.,0,0,0,0,1.]

def pack_glb(doc,blob):
    encoded=json.dumps(doc,separators=(',',':')).encode();encoded+=b' '*((-len(encoded))%4)
    blob=bytes(blob)+b'\0'*((-len(blob))%4)
    return struct.pack('<4sII',b'glTF',2,28+len(encoded)+len(blob))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(blob),0x004e4942)+blob

def glb_document():
    blob=bytearray();doc={'asset':{'version':'2.0','generator':'Gameengine CC0 fixture'},'buffers':[{}],'bufferViews':[],'accessors':[]}
    def accessor(values,kind,ct=5126):
        while len(blob)%4:blob.append(0)
        start=len(blob);fmt={5126:'f',5123:'H',5121:'B'}[ct]
        for row in values:blob.extend(struct.pack('<'+fmt*len(row),*row))
        view=len(doc['bufferViews']);doc['bufferViews'].append({'buffer':0,'byteOffset':start,'byteLength':len(blob)-start})
        i=len(doc['accessors']);doc['accessors'].append({'bufferView':view,'componentType':ct,'type':kind,'count':len(values)});return i
    pos=accessor(POSITIONS,'VEC3');ids=accessor([[i] for i in INDICES],'SCALAR',5123)
    joints=accessor([[0,1,0,0]]*6,'VEC4',5121);weights=accessor([[1,0,0,0]]*2+[[.5,.5,0,0]]*2+[[0,1,0,0]]*2,'VEC4')
    inverse=IDENTITY.copy();inverse[13]=-1
    ibm=accessor([IDENTITY,inverse],'MAT4');times=accessor([[0],[1],[2]],'SCALAR')
    rotations=accessor([[0,0,0,1],[0,0,math.sin(math.pi/8),math.cos(math.pi/8)],[0,0,0,1]],'VEC4')
    translations=accessor([[0,0,0],[1,0,0],[0,0,0]],'VEC3');lift=accessor([[0,0,0],[0,1,0],[0,0,0]],'VEC3')
    doc.update(scene=0,scenes=[{'nodes':[0,1]}],nodes=[{'mesh':0,'skin':0},{'name':'Root','children':[2]},{'name':'Tip','translation':[0,1,0]}],
        meshes=[{'primitives':[{'attributes':{'POSITION':pos,'JOINTS_0':joints,'WEIGHTS_0':weights},'indices':ids}]}],skins=[{'joints':[1,2],'inverseBindMatrices':ibm}],
        animations=[{'name':'Swing','samplers':[{'input':times,'output':rotations},{'input':times,'output':translations}],
            'channels':[{'sampler':0,'target':{'node':2,'path':'rotation'}},{'sampler':1,'target':{'node':1,'path':'translation'}}]},
            {'name':'Lift','samplers':[{'input':times,'output':lift}], 'channels':[{'sampler':0,'target':{'node':1,'path':'translation'}}]}])
    doc['buffers'][0]['byteLength']=len(blob);return doc,blob

def fbx_ascii():
    # The FBX transform conventions are also verified against the equivalent GLB.
    vertices=','.join(str(x) for p in POSITIONS for x in p)
    polygons=','.join(str(-v-1 if i%3==2 else v) for i,v in enumerate(INDICES))
    identity=','.join(str(x) for x in IDENTITY);tip=IDENTITY.copy();tip[13]=1;tip=','.join(str(x) for x in tip)
    objects=[f'Geometry: 100, "Geometry::Ribbon", "Mesh" {{\n Vertices: *18 {{ a: {vertices} }}\n PolygonVertexIndex: *12 {{ a: {polygons} }}\n}}',
        'Model: 200, "Model::Ribbon", "Mesh" { Version: 232 }',
        'Model: 201, "Model::Root", "LimbNode" { Version: 232 }',
        'Model: 202, "Model::Tip", "LimbNode" {\n Version: 232\n Properties70: { P: "Lcl Translation", "Lcl Translation", "", "A",0,1,0 }\n}',
        'Deformer: 300, "Deformer::Skin", "Skin" { Version: 101 }']
    for id,verts,weights,link in [(301,[0,1,2,3],[1,1,.5,.5],identity),(302,[2,3,4,5],[.5,.5,1,1],tip)]:
        bind=IDENTITY.copy();bind[13]=-1 if id==302 else 0;bind=','.join(map(str,bind))
        objects.append(f'Deformer: {id}, "SubDeformer::Cluster", "Cluster" {{\n Version: 100\n Indexes: *4 {{ a: '+','.join(map(str,verts))+' }\n Weights: *4 { a: '+','.join(map(str,weights))+f' }}\n Transform: *16 {{ a: {bind} }}\n TransformLink: *16 {{ a: {link} }}\n}}')
    objects += ['AnimationStack: 400, "AnimStack::Swing", "" {\n Properties70: {\n P: "LocalStart", "KTime", "Time", "",0\n P: "LocalStop", "KTime", "Time", "",92372316000\n }\n}', 'AnimationLayer: 401, "AnimLayer::BaseLayer", "" { }']
    connections=['C: "OO",100,200','C: "OO",200,0','C: "OO",201,0','C: "OO",202,201','C: "OO",300,100','C: "OO",301,300','C: "OO",302,300','C: "OO",201,301','C: "OO",202,302','C: "OO",401,400']
    for node,curve,bone,prop,axis,vals in [(410,420,202,'Lcl Rotation','Z','0,45,0'),(411,421,201,'Lcl Translation','X','0,1,0')]:
        objects.append(f'AnimationCurveNode: {node}, "AnimCurveNode::TRS", "" {{\n Properties70: {{ P: "d|{axis}", "Number", "", "A",0 }}\n}}')
        objects.append(f'AnimationCurve: {curve}, "AnimCurve::Curve", "" {{\n Default: 0\n KeyVer: 4008\n KeyTime: *3 {{ a: 0,46186158000,92372316000 }}\n KeyValueFloat: *3 {{ a: {vals} }}\n KeyAttrFlags: *3 {{ a: 4,4,4 }}\n KeyAttrDataFloat: *12 {{ a: 0,0,0,0,0,0,0,0,0,0,0,0 }}\n KeyAttrRefCount: *3 {{ a: 1,1,1 }}\n}}')
        connections += [f'C: "OO",{node},401',f'C: "OP",{node},{bone},"{prop}"',f'C: "OP",{curve},{node},"d|{axis}"']
    return '; FBX 7.4.0 project file; CC0 Gameengine two-bone fixture\nFBXHeaderExtension: {\n FBXHeaderVersion: 1003\n FBXVersion: 7400\n}\nGlobalSettings: {\n Version: 1000\n Properties70: {\n P: "UpAxis", "int", "Integer", "",1\n P: "UpAxisSign", "int", "Integer", "",1\n P: "FrontAxis", "int", "Integer", "",2\n P: "FrontAxisSign", "int", "Integer", "",-1\n P: "CoordAxis", "int", "Integer", "",0\n P: "CoordAxisSign", "int", "Integer", "",1\n P: "UnitScaleFactor", "double", "Number", "",100\n }\n}\nObjects: {\n'+'\n'.join(objects)+'\n}\nConnections: {\n'+'\n'.join(connections)+'\n}\n'

def fbx_binary(text):
    # Minimal ASCII-to-binary encoder for this self-authored fixture, not an importer.
    import re
    tokens=re.findall(r'"(?:\\.|[^"\\])*"|[^\S\n]+|\n|[A-Za-z_][\w| ]*|[-+]?\d+(?:\.\d+)?|[{}:,*]',re.sub(r';[^\n]*','',text))
    tokens=[x.strip() if x!='\n' else x for x in tokens if x=='\n' or x.strip()];at=0
    def nodes():
        nonlocal at
        result=[]
        while at<len(tokens):
            if tokens[at]=='\n':at+=1;continue
            if tokens[at]=='}':at+=1;break
            name=tokens[at];at+=1;assert tokens[at]==':';at+=1;props=[];children=[]
            while at<len(tokens) and tokens[at] not in ('\n','{','}'):
                t=tokens[at];at+=1
                if t==',':continue
                if t=='*':props.append(('array',int(tokens[at])));at+=1
                elif t.startswith('"'):props.append(json.loads(t))
                else:props.append(float(t) if '.' in t else int(t))
            if at<len(tokens) and tokens[at]=='{':at+=1;children=nodes()
            if props and isinstance(props[0],tuple):
                vals=children[0][1];assert props[0][1]==len(vals)
                props=[('values',name,vals)];children=[]
            result.append((name,props,children))
        return result
    tree=nodes()
    def prop(v):
        if isinstance(v,str):
            v=(v.split('::',1)[1]+'\0\x01'+v.split('::',1)[0]) if '::' in v else v
            data=v.encode();return b'S'+struct.pack('<I',len(data))+data
        if isinstance(v,tuple):
            _,name,values=v;fmt='l' if name=='KeyTime' else ('i' if name in ('Indexes','PolygonVertexIndex','KeyAttrFlags','KeyAttrRefCount') else 'd')
            data=struct.pack('<'+{'l':'q','i':'i','d':'d'}[fmt]*len(values),*values)
            return fmt.encode()+struct.pack('<III',len(values),0,len(data))+data
        return b'D'+struct.pack('<d',v) if isinstance(v,float) else b'L'+struct.pack('<q',v)
    def encode(node,offset):
        name,props,children=node;name=name.encode();body=b''.join(prop(v) for v in props)
        cursor=offset+13+len(name)+len(body);childbytes=b''
        for child in children:
            data=encode(child,cursor);childbytes+=data;cursor+=len(data)
        childbytes+=b'\0'*13;cursor+=13
        return struct.pack('<IIIB',cursor,len(props),len(body),len(name))+name+body+childbytes
    out=b'Kaydara FBX Binary  \0\x1a\0'+struct.pack('<I',7400)
    for node in tree:out+=encode(node,len(out))
    return out+b'\0'*13

if __name__=='__main__':
    root=Path(__file__).resolve().parents[2];folder=root/'assets/models/creator'
    doc,blob=glb_document()
    for name,data in [('two-bone.glb',pack_glb(doc,blob)),('two-bone.fbx',fbx_ascii().encode()),('two-bone-binary.fbx',fbx_binary(fbx_ascii()))]:
        path=folder/name
        if not path.exists() or path.read_bytes()!=data:path.write_bytes(data)
