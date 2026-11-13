#include "OpticsCommon.hlsli"
// A single straight straw in an open, water-filled tumbler.
// Analytic first-hit intersections; trace transmission across air/glass/water interfaces.
static const float outerRadius = 1.0, innerRadius = .922;
static const float outerBottom = -1.13, innerBottom = -.995, rimY = 1.035, waterY = .02;
static const float3 strawBase = float3(.27,-.92,.73), strawTip = float3(1.02,1.76,.045);
static const float strawRadius = .070;
static const float infDistance = 1e6, stepEpsilon = .00025;
static const uint air = 0, glass = 1, water = 2;

uint Medium(float3 p) {
    float r2 = dot(p.xz,p.xz);
    if (p.y < outerBottom || p.y > rimY || r2 > outerRadius*outerRadius) return air;
    if (p.y < innerBottom || r2 > innerRadius*innerRadius) return glass;
    return p.y < waterY ? water : air;
}
float IndexOf(uint medium) { return medium==glass ? PLATE_IOR : (medium==water ? 1.333 : 1.0); }
float3 Absorption(uint medium) {
    if (medium==water) return PLATE_ABSORPTION*float3(.18,.048,.016);
    if (medium==glass) return PLATE_ABSORPTION*float3(.22,.04,.09);
    return 0;
}
struct Hit { float t; float3 n; };
void Keep(inout Hit hit, float t, float3 n) {
    if (t > stepEpsilon && t < hit.t) { hit.t=t; hit.n=n; }
}
void VerticalWall(inout Hit hit, float3 o,float3 d,float radius,float low,float high) {
    float a=dot(d.xz,d.xz), b=dot(o.xz,d.xz), c=dot(o.xz,o.xz)-radius*radius;
    float disc=b*b-a*c;
    if (disc<0 || a<1e-8) return;
    float2 ts=(-b+float2(-1,1)*sqrt(disc))/a;
    [unroll]for(int i=0;i<2;i++){
        float3 p=o+ts[i]*d;
        if(p.y>=low && p.y<=high)Keep(hit,ts[i],float3(p.x/radius,0,p.z/radius));
    }
}
void HorizontalFace(inout Hit hit,float3 o,float3 d,float y,float rMin,float rMax) {
    if(abs(d.y)<1e-7)return;
    float t=(y-o.y)/d.y;float2 p=o.xz+t*d.xz;float q=dot(p,p);
    if(q>=rMin*rMin && q<=rMax*rMax)Keep(hit,t,float3(0,1,0));
}
Hit CupBoundary(float3 o,float3 d){
    Hit hit;hit.t=infDistance;hit.n=0;
    VerticalWall(hit,o,d,outerRadius,outerBottom,rimY);
    VerticalWall(hit,o,d,innerRadius,innerBottom,rimY);
    HorizontalFace(hit,o,d,outerBottom,0,outerRadius);
    HorizontalFace(hit,o,d,innerBottom,0,innerRadius);
    HorizontalFace(hit,o,d,rimY,innerRadius,outerRadius);
    HorizontalFace(hit,o,d,waterY,0,innerRadius);
    return hit;
}
Hit StrawHit(float3 o,float3 d){
    Hit h;h.t=infDistance;h.n=0;
    float3 axis=normalize(strawTip-strawBase), m=o-strawBase;
    float lengthS=length(strawTip-strawBase), od=dot(m,axis), dd=dot(d,axis);
    float3 mp=m-od*axis, dp=d-dd*axis;
    float a=dot(dp,dp),b=dot(mp,dp),c=dot(mp,mp)-strawRadius*strawRadius;
    float disc=b*b-a*c;
    if(disc>=0 && a>1e-8){
        float2 ts=(-b+float2(-1,1)*sqrt(disc))/a;
        [unroll]for(int j=0;j<2;j++){
            float s=od+ts[j]*dd;
            if(s>=0 && s<=lengthS)Keep(h,ts[j],normalize(mp+ts[j]*dp));
        }
    }
    if(abs(dd)>1e-7){
        [unroll]for(int j=0;j<2;j++){
            float t=(j*lengthS-od)/dd;float3 q=mp+t*dp;
            if(dot(q,q)<=strawRadius*strawRadius)Keep(h,t,(j?1:-1)*axis);
        }
    }
    return h;
}
float3 StrawColor(float3 p,float3 n,float3 view){
    float3 axis=normalize(strawTip-strawBase);float distanceS=dot(p-strawBase,axis);
    // The straw remains solid red at the waterline, with two white bands near its mouth.
    float3 pigment=float3(.90,.022,.038);
    if(distanceS>2.32 && distanceS<2.40 || distanceS>2.52 && distanceS<2.57)
        pigment=float3(.98,.82,.68);
    float brightness=.44+.56*saturate(dot(n,normalize(float3(-.6,.8,-.55))));
    float highlight=.17*pow(saturate(dot(n,normalize(normalize(float3(-.6,.8,-.55))+view))),30);
    if(dot(n,axis)>.98 && length(p-strawTip)<strawRadius*.77)return float3(.12,.008,.016);
    return pigment*brightness+highlight;
}
float3 Studio(float3 d){
    float t=saturate(.60+d.y*.48);
    float3 sky=lerp(float3(.26,.38,.46),float3(.83,.92,.95),t);
    float stripe=smoothstep(.90,.99,dot(d,normalize(float3(-.78,.20,-.48))));
    float dark=smoothstep(.83,.97,dot(d,normalize(float3(.73,.1,-.60))));
    return lerp(sky,float3(.033,.068,.093),dark) + stripe*float3(.9,.96,1);
}
float3 Background(float3 o,float3 d){
    if(d.y<-.0001){
        float t=(outerBottom-.015-o.y)/d.y;
        if(t>stepEpsilon){
            float2 p=o.xz+t*d.xz;
            float contact=.32*exp(-dot(p*float2(.87,.80),p*float2(.87,.80)));
            return float3(.84,.79,.68)*(1-contact);
        }
    }
    return lerp(float3(.84,.79,.68),float3(.66,.77,.82),saturate(d.y*2.8+.1));
}
float3 RenderCup(float2 ndc){
    float3 eye=float3(0,1.53,-5.85), forward=normalize(float3(0,.27,0)-eye);
    float3 right=float3(1,0,0), up=cross(forward,right);
    float3 o=eye,d=normalize(forward+right*ndc.x*.57+up*ndc.y*(.57*9/16));
    float3 color=0,weight=1;
    [loop]for(int bounce=0;bounce<12;bounce++){
        Hit wall=CupBoundary(o,d), straw=StrawHit(o,d);
        uint medium=Medium(o);
        float travel=min(wall.t,straw.t);
        if(travel==infDistance){return color+weight*Background(o,d);}
        weight*=exp(-Absorption(medium)*travel);
        float3 hit=o+d*travel;
        if(straw.t<=wall.t)return color+weight*StrawColor(hit,straw.n,-d);
        float3 normal=dot(wall.n,d)>0 ? -wall.n : wall.n;
        uint nextMedium=Medium(hit+d*stepEpsilon*4);
        if(nextMedium==medium){o=hit+d*stepEpsilon*3;continue;}
        float etaI=IndexOf(medium),etaT=IndexOf(nextMedium),cosI=-dot(d,normal);
        float3 transmitted=refract(d,normal,etaI/etaT);
        if(dot(transmitted,transmitted)<1e-8){
            #if STRAW_BEND
            return color + weight*Studio(reflect(d,normal));
#endif
            o=hit+d*stepEpsilon*3;continue;
        }
        float fresnel=FresnelDielectric(cosI,-dot(transmitted,normal),etaI,etaT);
        color+=weight*fresnel*Studio(reflect(d,normal));
        weight*=1-fresnel;
#if STRAW_BEND
        d=normalize(transmitted);
#endif
        o=hit+d*stepEpsilon*3;
    }
    return color+weight*Studio(d);
}
float4 PSMain(ScreenVertex input):SV_Target{
    return float4(pow(saturate(RenderCup(input.ndc)),1.0/2.2),1);
}
