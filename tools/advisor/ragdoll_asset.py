"""Build-only bounded ragdoll proxies in the initial skinned model coordinate system."""
import math
import struct

def cook(asset,scale,used):
    rig=asset.get('rig',[]);count=len(asset['frames'][0])
    if not 2<=count<=32 or len(rig)!=count:raise ValueError('Ragdoll requires 2..32 palette bones with hierarchy')
    if len({b['node'] for b in rig})!=count:raise ValueError('Ragdoll requires unique palette bones; merge duplicate skin instances first')
    parents=[b['parent'] for b in rig]
    if not any(p>=0 for p in parents):raise ValueError('Ragdoll requires a parent-child joint')
    for i in range(count):
        seen=set();j=i
        while j!=-1:
            if type(j)!=int or not 0<=j<count or j in seen:raise ValueError('Invalid/cyclic ragdoll hierarchy')
            seen.add(j);j=parents[j]
    data=bytearray(b'GERIG1\0\0'+struct.pack('<I',count))
    for j,bone in enumerate(rig):
        anchor=[float(x)*scale for x in bone['anchor']]
        if len(anchor)!=3 or any(not math.isfinite(x) or abs(x)>10000 for x in anchor):raise ValueError('Invalid ragdoll joint anchor')
        points=[asset['positions'][i] for i in used if any(k==j and w>=.1 for k,w in zip(asset['joints'][i],asset['weights'][i]))]
        if points:
            low=[min(p[a]*scale for p in points) for a in range(3)];high=[max(p[a]*scale for p in points) for a in range(3)]
            center=[(a+b)*.5 for a,b in zip(low,high)];size=[max(.1,b-a+.02) for a,b in zip(low,high)]
        else:center=anchor;size=[.1]*3
        if any(not math.isfinite(x) or abs(x)>10000 for x in center) or any(not .1<=x<=20 for x in size):raise ValueError('Ragdoll proxy exceeds 20 meter extent or invalid center')
        data+=struct.pack('<i9f',bone['parent'],*center,*size,*anchor)
    return bytes(data)
