#ifndef ENGINE_RAY_TRACING
#define ENGINE_RAY_TRACING 0
#endif
#if ENGINE_RAY_TRACING
struct RayBox {
    row_major float4x4 inverse; float4 color;
#if ENGINE_RT_MESH
    uint4 geometry;
#endif
};
#if ENGINE_RT_MESH
struct RayTriangle {float3 a;uint pad0;float3 b;uint pad1;float3 c;uint pad2;float3 normal;uint pad3;};
struct RayNode {float3 minimum;uint first;float3 maximum;uint count;};
StructuredBuffer<RayTriangle> rayTriangles : register(t5);
StructuredBuffer<RayNode> rayNodes : register(t6);
#endif
cbuffer RaySceneConstants : register(b1) { uint rayBoxCount; uint3 rayPadding; RayBox rayBoxes[32]; };
#if ENGINE_RT_HARDWARE
RaytracingAccelerationStructure rayScene : register(t4);
#endif
// Returns the first positive boundary, including rays starting inside a box.
bool BoxIntersection(float3 origin,float3 direction,uint index,out float t) {
    float3 o=mul(float4(origin,1),rayBoxes[index].inverse).xyz;
    float3 d=mul(float4(direction,0),rayBoxes[index].inverse).xyz;
    float nearT=-1e30,farT=1e30;
    [unroll] for(int axis=0;axis<3;++axis) {
        if(abs(d[axis])<1e-8) {if(abs(o[axis])>.5){t=0;return false;}}
        else {float a=(-.5-o[axis])/d[axis],b=(.5-o[axis])/d[axis];nearT=max(nearT,min(a,b));farT=min(farT,max(a,b));}
    }
    t=nearT>=.001?nearT:farT;
    return farT>=nearT && t>=.001;
}
float3 BoxNormal(float3 position,uint index) {
    float3 p=mul(float4(position,1),rayBoxes[index].inverse).xyz;
    float3 a=abs(p),n=0;
    if(a.x>=a.y&&a.x>=a.z)n.x=p.x>=0?1:-1;
    else if(a.y>=a.z)n.y=p.y>=0?1:-1;else n.z=p.z>=0?1:-1;
    return normalize(mul((float3x3)rayBoxes[index].inverse,n));
}
#if ENGINE_RT_MESH
bool NodeIntersection(float3 origin,float3 direction,RayNode node,float maximum){
    float low=.001,high=maximum;
    [unroll] for(uint axis=0;axis<3;++axis){
        if(abs(direction[axis])<1e-8){if(origin[axis]<node.minimum[axis]||origin[axis]>node.maximum[axis])return false;}
        else {float a=(node.minimum[axis]-origin[axis])/direction[axis],b=(node.maximum[axis]-origin[axis])/direction[axis];low=max(low,min(a,b));high=min(high,max(a,b));}
    }
    return high>=low;
}
bool TriangleIntersection(float3 origin,float3 direction,uint index,out float t){
    RayTriangle tri=rayTriangles[index];float3 e1=tri.b-tri.a,e2=tri.c-tri.a,p=cross(direction,e2);
    float det=dot(e1,p);t=0;if(abs(det)<1e-8)return false;
    float inv=1/det;float3 v=origin-tri.a;float u=dot(v,p)*inv;if(u<0||u>1)return false;
    float3 q=cross(v,e1);float w=dot(direction,q)*inv;if(w<0||u+w>1)return false;
    t=dot(e2,q)*inv;return t>=.001;
}
bool MeshIntersection(float3 origin,float3 direction,uint instance,float maximum,out float t,out uint primitive){
    float3 o=mul(float4(origin,1),rayBoxes[instance].inverse).xyz;
    float3 d=mul(float4(direction,0),rayBoxes[instance].inverse).xyz;
    // Do not normalize d: the ray parameter must remain in world-space units.
    t=maximum;primitive=0xffffffff;uint stack[32];uint size=1;stack[0]=rayBoxes[instance].geometry.z;
    [loop] while(size){
        RayNode node=rayNodes[stack[--size]];if(!NodeIntersection(o,d,node,t))continue;
        if(node.count){[loop] for(uint j=0;j<node.count;++j){uint id=node.first+j;float hit;if(TriangleIntersection(o,d,id,hit)&&hit<t){t=hit;primitive=id;}}}
        else {stack[size++]=node.first;stack[size++]=node.first+1;}
    }
    return primitive!=0xffffffff;
}
#endif
bool SceneIntersection(float3 origin,float3 direction,float maximum,out float t,out float3 normal,out float3 color) {
    t=maximum;normal=0;color=0;uint hit=32;
#if ENGINE_RT_MESH
    uint primitive=0xffffffff;
#endif
#if ENGINE_RT_HARDWARE
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    RayDesc ray;ray.Origin=origin;ray.Direction=direction;ray.TMin=.001;ray.TMax=maximum;
    query.TraceRayInline(rayScene,RAY_FLAG_NONE,255,ray);while(query.Proceed()){}
    if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
        hit=query.CommittedInstanceID();t=query.CommittedRayT();
#if ENGINE_RT_MESH
        if(rayBoxes[hit].geometry.x)primitive=rayBoxes[hit].geometry.y+query.CommittedPrimitiveIndex();
#endif
    }
#else
    [loop] for(uint i=0;i<rayBoxCount;++i){
        float candidate;bool intersects;
#if ENGINE_RT_MESH
        uint candidateTriangle=0xffffffff;
        if(rayBoxes[i].geometry.x)intersects=MeshIntersection(origin,direction,i,t,candidate,candidateTriangle);
        else
#endif
        intersects=BoxIntersection(origin,direction,i,candidate);
        if(intersects&&candidate<t){t=candidate;hit=i;
#if ENGINE_RT_MESH
            primitive=candidateTriangle;
#endif
        }
    }
#endif
    if(hit>=rayBoxCount)return false;
#if ENGINE_RT_MESH
    if(rayBoxes[hit].geometry.x){
        normal=normalize(mul((float3x3)rayBoxes[hit].inverse,rayTriangles[primitive].normal));
        if(dot(normal,direction)>0)normal=-normal; // Opaque, two-sided RT meshes.
    }else
#endif
    normal=BoxNormal(origin+direction*t,hit);
    color=rayBoxes[hit].color.rgb;return true;
}
float RayShadow(float3 position,float3 normal,float3 toLight){float t;float3 n,c;return SceneIntersection(position+normal*.003,toLight,10000,t,n,c)?0:1;}
float3 RayReflection(float3 position,float3 normal,float3 view,float3 toLight,float3 light,float3 ambient) {
    float3 dir=reflect(-view,normal);float t;float3 n,c;
    if(!SceneIntersection(position+normal*.003,dir,10000,t,n,c))return float3(.15,.22,.35);
    float3 hit=position+normal*.003+dir*t;
    // One reflected ray plus a shadow ray at its hit; no recursive GI or roughness convolution.
    return c*(ambient+light*saturate(dot(n,toLight))*RayShadow(hit,n,toLight));
}
#endif
