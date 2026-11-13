struct ScreenVertex {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};
ScreenVertex VSMain(uint vertexId : SV_VertexID) {
    // The creator sample submits two indexed quads for its HUD.
    const float2 corners[4] = {float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,1)};
    ScreenVertex o;
    o.ndc = vertexId < 4 ? corners[vertexId % 4] : float2(0,0);
    o.position = float4(o.ndc,0,1);
    return o;
}
float FresnelDielectric(float cosI,float cosT,float etaI,float etaT) {
    float rs=(etaI*cosI-etaT*cosT)/max(etaI*cosI+etaT*cosT,1e-7);
    float rp=(etaT*cosI-etaI*cosT)/max(etaT*cosI+etaI*cosT,1e-7);
    return .5*(rs*rs+rp*rp);
}
