#include "OpticsCommon.hlsli"

// Fixed optical study: the primary camera ray and a single mirror ray intersect
// the same room geometry. Only the two box pigments change in the control build.
static const float3 sphereCenter = float3(0,1.15,0);
static const float sphereRadius = 1.15;
static const float3 lampPosition = float3(-2.6,5.6,-3.5);
static const float3 redPigment = float3(.92,.032,.018);
static const float3 bluePigment = float3(.016,.20,.92);
static const float pi = 3.14159265;
static const int room = 0, redBox = 1, blueBox = 2, metal = 3;

struct Hit { float t; float3 normal; int objectId; };

void Box(inout Hit hit, float3 origin, float3 direction, float3 low, float3 high, int id) {
    float3 signD = float3(direction.x < 0 ? -1:1, direction.y < 0 ? -1:1, direction.z < 0 ? -1:1);
    float3 inv = signD / max(abs(direction),1e-7);
    float3 lo = (low-origin)*inv, hi = (high-origin)*inv;
    float3 entry = min(lo,hi), exitAt = max(lo,hi);
    float nearT = max(entry.x,max(entry.y,entry.z)), farT = min(exitAt.x,min(exitAt.y,exitAt.z));
    if (farT < nearT || farT < .0001) return;
    float t = nearT > .0001 ? nearT : farT;
    if (t >= hit.t) return;
    float3 times = nearT > .0001 ? entry : exitAt;
    float3 axis = (times.x == t) ? float3(1,0,0) : ((times.y == t) ? float3(0,1,0) : float3(0,0,1));
    hit.t = t; hit.normal = -signD*axis; hit.objectId = id;
}

void Sphere(inout Hit hit, float3 origin, float3 direction) {
    float3 offset = origin-sphereCenter;
    float b = dot(offset,direction);
    float disc = b*b-dot(offset,offset)+sphereRadius*sphereRadius;
    if (disc <= 0) return;
    float t = -b-sqrt(disc);
    if (t > .0001 && t < hit.t) {
        hit.t = t;
        hit.normal = normalize(offset+t*direction);
        hit.objectId = metal;
    }
}

Hit Trace(float3 origin,float3 direction,bool sphereVisible) {
    Hit hit; hit.t = 1e5; hit.normal = 0; hit.objectId = room;
    Box(hit,origin,direction,float3(-5,0,-8),float3(5,6,4.3),room);
    Box(hit,origin,direction,float3(-2.38,0,-1.34),float3(-1.33,2.14,.16),redBox);
    Box(hit,origin,direction,float3(1.30,0,-1.58),float3(2.45,1.55,.05),blueBox);
    if (sphereVisible) Sphere(hit,origin,direction);
    return hit;
}

float3 RoomPigment(float3 p,float3 n) {
    if (n.y > .9) {
        float tile = fmod(floor(p.x*1.05)+floor(p.z*1.05)+40,2);
        return lerp(float3(.51,.54,.58),float3(.76,.77,.78),tile);
    }
    if (n.y < -.9) return float3(.76,.80,.86);
    // A charcoal front wall and two tall softboxes give the metal a recognizable
    // dark-to-light contour. They are geometric room features, fixed in world space.
    if (n.z > .9) return float3(.065,.084,.11);
    float top = saturate(p.y/5.5);
    return lerp(float3(.29,.32,.37),float3(.68,.72,.78),top);
}

float Softbox(float3 p,float3 n) {
    if (n.z > .9 && p.y > 1.5 && p.y < 5.6 &&
        ((p.x > -4 && p.x < -2.55) || (p.x > 2.6 && p.x < 3.45))) return 1.4;
    if (n.y < -.9 && abs(p.x)<1.8 && abs(p.z+.5)<1.45) return 1.5;
    return 0;
}

float3 MatteSurface(Hit hit,float3 p) {
    float3 pigment = RoomPigment(p,hit.normal);
    if (hit.objectId == redBox) pigment = SWAP_BOX_COLORS ? bluePigment : redPigment;
    if (hit.objectId == blueBox) pigment = SWAP_BOX_COLORS ? redPigment : bluePigment;
    float3 toLight = lampPosition-p;
    float distanceToLight = length(toLight);
    float3 l = toLight/distanceToLight;
    float ndl = saturate(dot(hit.normal,l));
    float visibility = 1;
    if (ndl > 0) {
        Hit blocker = Trace(p+hit.normal*.002,l,true);
        visibility = blocker.t < distanceToLight ? .18 : 1;
    }
    float contact = 1;
    if (hit.objectId == room && hit.normal.y > .9)
        contact -= .36*exp(-dot(p.xz,p.xz)*1.7);
    return pigment * (.33 + .79*ndl*visibility) * contact +
           (hit.objectId == room ? Softbox(p,hit.normal) : 0);
}

float SmithG1(float nDotV,float alphaSquared) {
    return 2*nDotV / max(nDotV+sqrt(alphaSquared+(1-alphaSquared)*nDotV*nDotV),1e-5);
}

float3 ShadeMetal(Hit hit,float3 p,float3 view) {
    float3 n=hit.normal;
    float3 mirror=reflect(-view,n);
    float3 start=p+.002*n;
    Hit reflected=Trace(start,mirror,false);
    float3 incoming=MatteSurface(reflected,start+reflected.t*mirror);
    float3 f0=float3(.91,.92,.94);
    float nv=saturate(dot(n,view));
    float3 viewFresnel=f0+(1-f0)*pow(1-nv,5);
    // Polished metal: use the mirror direction for reflected scene radiance,
    // and a GGX / Smith microfacet term for the direct lamp highlight.
    float3 l=normalize(lampPosition-p), h=normalize(l+view);
    float nl=saturate(dot(n,l)), nh=saturate(dot(n,h)), vh=saturate(dot(view,h));
    const float roughness=.16;
    float alpha=roughness*roughness, a2=alpha*alpha;
    float denom=nh*nh*(a2-1)+1;
    float D=a2/max(pi*denom*denom,1e-7);
    float G=SmithG1(nv,a2)*SmithG1(nl,a2);
    float3 F=f0+(1-f0)*pow(1-vh,5);
    return incoming*viewFresnel + .36*D*G*F*nl/max(4*nv*nl,.0001);
}

float4 PSMain(ScreenVertex input):SV_Target {
    float3 eye=float3(0,2.38,-6.85);
    float3 forward=normalize(float3(0,1.15,0)-eye);
    float3 up=cross(forward,float3(1,0,0));
    float3 dir=normalize(forward+float3(input.ndc.x*.60,0,0)+up*(input.ndc.y*.60*9/16));
    Hit hit=Trace(eye,dir,true);
    float3 p=eye+hit.t*dir;
    float3 color=hit.objectId==metal ? ShadeMetal(hit,p,-dir) : MatteSurface(hit,p);
    return float4(pow(saturate(color),1.0/2.2),1);
}

