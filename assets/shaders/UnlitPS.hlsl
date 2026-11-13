// Same vertex interface as SimpleVS; no lighting, textures, or shadow samples.
struct PSInput {
    float4 position : SV_Position;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};
float4 PSMain(PSInput input) : SV_Target { return input.color; }
