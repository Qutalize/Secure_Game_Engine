// スキン付きメッシュのシャドウパス用頂点シェーダー（ステージL-04b）。
//
// なぜ ShadowVS.hlsl と別にするのか:
//   ShadowVS.hlsl は「POSITION だけの入力レイアウト」で Vertex(40B)/TexVertex(32B) の
//   両方を 1 つの PSO で描けるようにしていた（先頭 12 バイトが position で共通なので）。
//   しかし SkinnedPbrVertex は頂点をスキン行列で変形してから位置を出す必要があり、
//   BLENDINDICES/BLENDWEIGHT も読まねばならない＝共通化できない。
//
// これが無いとどうなるか:
//   キャラだけ影が「バインドポーズ（T ポーズ）の形」で落ち、本体の動きに追従しない。
//   歩いているのに影だけ直立、という違和感の強い見た目になる。

cbuffer SceneConstants : register(b0)
{
    float4x4 gView;          // シャドウパスではライト視点のビュー行列
    float4x4 gProj;          // 〃 ライトの正射影行列
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

cbuffer SkinningConstants : register(b1)
{
    float4x4 gJoints[128];
};

struct VSInput
{
    float3 position : POSITION;
    uint4  joints   : BLENDINDICES;
    float4 weights  : BLENDWEIGHT;
};

float4x4 SkinMatrix(uint4 j, float4 w)
{
    return gJoints[j.x] * w.x + gJoints[j.y] * w.y +
           gJoints[j.z] * w.z + gJoints[j.w] * w.w;
}

float4 VSMain(VSInput input) : SV_Position
{
    float4x4 skin = SkinMatrix(input.joints, input.weights);
    float4 skinnedPos = mul(float4(input.position, 1.0f), skin);
    float4 wp = mul(skinnedPos, gWorld);
    return mul(mul(wp, gView), gProj);
}
