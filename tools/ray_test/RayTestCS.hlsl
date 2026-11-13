#include "../../assets/shaders/RayTracing.hlsli"
RWStructuredBuffer<float4> results : register(u0);
[numthreads(8,1,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint i=id.x;float t;float3 n,c;
    float3 o=float3(0,0,-3),d=float3(0,0,1);float maximum=100;
#if ENGINE_RT_MESH
    if(i>=8){
        o=float3(9,0,0);
        if(i==9)o.x=10;
        if(i==10){o.z=8;d.z=-1;}
        if(i==11||i==12){o=float3(12,0,i==11?6:4);d=float3(1,0,0);}
        if(i==13){results[i]=float4(RayReflection(o,float3(0,0,-1),d,-d,1,.25),1);return;}
        if(i==14){results[i]=RayShadow(float3(10,0,0),float3(0,0,-1),d).xxxx;return;}
        if(i==15)o.x=20;
        bool hit=SceneIntersection(o,d,maximum,t,n,c);results[i]=float4(hit?t:-1,n);return;
    }
#endif
    if(i==1)o.y=2;
    if(i==2){o=0;d=float3(1,0,0);}
    if(i==3)o.x=3;
    if(i==4)maximum=1;
    if(i==5){results[i]=RayShadow(o,float3(0,0,-1),d).xxxx;return;}
    if(i==6){results[i]=float4(RayReflection(o,float3(0,0,-1),d,-d,1,.25),1);return;}
    if(i==7){results[i]=RayShadow(float3(0,2,-3),float3(0,0,-1),d).xxxx;return;}
    bool hit=SceneIntersection(o,d,maximum,t,n,c);results[i]=float4(hit?t:-1,n);
}
