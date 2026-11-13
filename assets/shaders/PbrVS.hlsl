// PBR 頂点シェーダー（ステージL-02）。
// POSITION + NORMAL + TANGENT(float4) + TEXCOORD を入力に取る。
//
// なぜ TANGENT が要るのか:
//   法線マップは「その面のローカル座標系（接空間）での法線の傾き」を RGB に焼いたもの。
//   これをワールド空間の法線に変換するには、面の「横方向(tangent)」「縦方向(bitangent)」
//   「面の向き(normal)」の 3 本の軸＝TBN 行列が要る。tangent は UV の u 方向に対応する
//   ワールド方向で、glTF が TANGENT 属性として持っている（無ければローダーが生成する）。
//   w 成分は bitangent の向き（+1 / -1）。UV がミラーリングされている面で符号が反転する。

cbuffer SceneConstants : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorld;
    float4   gLightDir;
    float4   gLightColor;
    float4   gAmbient;
    float4   gCameraPos;
    float4x4 gLightViewProj;
    float4   gShadowParams;
    // ステージL-02 で追加。C++ 側 SceneConstants とメンバ順を一致させること。
    float4   gBaseColorFactor;  // rgba: baseColor 係数
    float4   gMaterialParams;   // x=metallic, y=roughness, z=法線マップ強度, w=テクスチャ有無フラグ
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;   // xyz=接線, w=従法線の符号
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position  : SV_Position;
    float3 worldPos  : POSITION;
    float3 normal    : NORMAL;
    float3 tangent   : TANGENT;
    float3 bitangent : BINORMAL;
    float2 uv        : TEXCOORD;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 wp = mul(float4(input.position, 1.0f), gWorld);
    o.worldPos = wp.xyz;
    o.position = mul(mul(wp, gView), gProj);

    // 回転＋一様スケール前提（既存経路と同じ制約）。
    float3 n = normalize(mul(input.normal, (float3x3)gWorld));
    float3 t = normalize(mul(input.tangent.xyz, (float3x3)gWorld));
    // グラム・シュミット直交化: 補間で t が n と直交でなくなるのを補正する。
    t = normalize(t - n * dot(n, t));

    o.normal    = n;
    o.tangent   = t;
    o.bitangent = cross(n, t) * input.tangent.w;
    o.uv        = input.uv;
    return o;
}
