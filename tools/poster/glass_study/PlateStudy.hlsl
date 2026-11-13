// Fixed-scene optical study, executed as a full-screen pass in SelectedGame.
// World units are metres; the pane is mounted in an opaque frame.
#include "OpticsCommon.hlsli"
float3 Backdrop(float3 origin,float3 direction) {
    // A striped board at z=3, with two colored vertical markers.
    if(direction.z > .05) {
        float3 hit=origin+direction*((3-origin.z)/direction.z);
        if(abs(hit.x)<6 && abs(hit.y)<4) {
            float band=frac((hit.x+2.0)/.90);
            float3 color=band<.15 ? float3(.027,.075,.13) : float3(.74,.79,.76);
            if(abs(hit.x+.49)<.18)color=float3(.87,.32,.065);
            if(abs(hit.x-.69)<.18)color=float3(.025,.48,.50);
            if(abs(frac((hit.y+3)/.92)-.5)<.015)color*=.67;
            return color;
        }
    }
    float sky=saturate(direction.y*.55+.55);
    return lerp(float3(.045,.073,.11),float3(.57,.76,.91),sky);
}
float3 RenderStudy(float2 ndc) {
    const float3 eye=float3(0,0,-4.0);
    float3 incident=normalize(float3(ndc.x*1.05,ndc.y*(1.05*9/16),1.60));
    float3 behind=Backdrop(eye,incident);
#if PLATE_BYPASS
    return behind;
#endif
    const float tilt=48.0*3.14159265/180.0;
    const float3 normal=float3(sin(tilt),0,-cos(tilt));
    const float3 along=float3(cos(tilt),0,sin(tilt));
    const float thickness=.55;
    const float2 bounds=float2(1.90,1.08);
    float cosIn=-dot(incident,normal);
    if(cosIn<=1e-5)return behind;
    float t=dot(-eye,normal)/dot(incident,normal);
    float3 front=eye+t*incident;
    float2 uv=float2(dot(front,along),front.y);
    if(t<=0 || any(abs(uv)>bounds))return behind;
    const float3 frameColor=float3(.025,.105,.135);
    if(any(abs(uv)>bounds-.025))return frameColor;
    float3 reflected=Backdrop(front,reflect(incident,normal));
    float3 inside=refract(incident,normal,1.0/PLATE_IOR);
    float cosInside=-dot(inside,normal);
    if(cosInside<=1e-6)return reflected;
    float travel=thickness/cosInside;
    float3 exitPoint=front+travel*inside;
    float frontF=FresnelDielectric(cosIn,cosInside,1,PLATE_IOR);
    if(any(abs(float2(dot(exitPoint,along),exitPoint.y))>bounds-.025))
        return lerp(frameColor,reflected,frontF);
    // At the back face the incident-medium-facing normal is again normal.
    float3 outgoing=refract(inside,normal,PLATE_IOR);
    if(dot(outgoing,outgoing)<1e-8)return reflected; // Not reached for this air/parallel-slab setup.
    float backF=FresnelDielectric(cosInside,-dot(outgoing,normal),PLATE_IOR,1);
    float3 transmitted=Backdrop(exitPoint,outgoing);
    // Beer-Lambert absorption. The reference setting uses zero absorption.
    const float3 absorption=PLATE_ABSORPTION*float3(.22,.055,.025);
    return frontF*reflected + (1-frontF)*(1-backF)*exp(-absorption*travel)*transmitted;
}
float4 PSMain(ScreenVertex input):SV_Target{
    return float4(pow(saturate(RenderStudy(input.ndc)),1.0/2.2),1);
}
