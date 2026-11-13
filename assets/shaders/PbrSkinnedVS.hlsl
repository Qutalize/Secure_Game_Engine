// スキン付き PBR 頂点シェーダー（ステージL-04b）。
// PbrVS.hlsl との違いは「頂点をワールド行列で変換する前に、まずボーンで変形する」こと。
//
// スキニングの計算:
//   頂点 1 個につき、影響するボーン最大 4 本の「行列」を重み付き平均して 1 本の
//   合成行列にし、その行列で頂点を変換する（線形ブレンドスキニング／LBS）。
//   これが最もシンプルなスキニング方式で、肘や膝の内側が潰れる（キャンディラップ現象）
//   という既知の弱点があるが、実装が単純で GPU コストも低い。より良い品質が要れば
//   デュアルクォータニオンスキニングがあるが、本プロジェクトの規模では LBS で十分。

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
    float4   gBaseColorFactor;
    float4   gMaterialParams;
};

// ジョイント行列パレット（ステージL-04b）。CPU 側で
//   joint = 逆バインド行列 × 現在の世界変換
// まで計算し、他の行列と同じく転置してアップロードしている。
cbuffer SkinningConstants : register(b1)
{
    float4x4 gJoints[128];
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    uint4  joints   : BLENDINDICES;
    float4 weights  : BLENDWEIGHT;
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

// 4 本のジョイント行列を重み付き平均した合成行列を返す。
// ★行列を平均してから使うので、個々のジョイント回転が大きく異なる関節では
//   厳密には正しくない（デュアルクォータニオンならこの問題が緩和される）が、
//   実用上は歩行・射撃程度のアニメーションで破綻しない。
float4x4 SkinMatrix(uint4 j, float4 w)
{
    return gJoints[j.x] * w.x + gJoints[j.y] * w.y +
           gJoints[j.z] * w.z + gJoints[j.w] * w.w;
}

VSOutput VSMain(VSInput input)
{
    VSOutput o;

    float4x4 skin = SkinMatrix(input.joints, input.weights);

    // 位置・法線・接線を「まずスキンで変形 → 次にワールド変換」の順で運ぶ。
    // スキン行列はモデル空間内の変形なので、ワールド変換より内側（先）に掛ける。
    float4 skinnedPos = mul(float4(input.position, 1.0f), skin);
    float3 skinnedNrm = mul(input.normal, (float3x3)skin);
    float3 skinnedTan = mul(input.tangent.xyz, (float3x3)skin);

    float4 wp = mul(skinnedPos, gWorld);
    o.worldPos = wp.xyz;
    o.position = mul(mul(wp, gView), gProj);

    float3 n = normalize(mul(skinnedNrm, (float3x3)gWorld));
    float3 t = normalize(mul(skinnedTan, (float3x3)gWorld));
    t = normalize(t - n * dot(n, t));   // グラム・シュミット直交化（PbrVS と同じ）

    o.normal    = n;
    o.tangent   = t;
    o.bitangent = cross(n, t) * input.tangent.w;
    o.uv        = input.uv;
    return o;
}
