// シャドウパス用の頂点シェーダー（ステージL-03b）。
//
// シャドウマップは「ライトの位置から見たシーンの深度」だけを記録したテクスチャ。
// このパスでは色を一切計算しないので、ピクセルシェーダーは付けない（深度専用パス）。
// GPU はラスタライズして深度を書き込むだけなので、本描画よりずっと速い。
//
// ★入力レイアウトの工夫★
// このシェーダーは POSITION しか使わない。DX12 の入力レイアウトは「頂点バッファの
// どのオフセットに何があるか」を宣言するもので、1 頂点のサイズ（ストライド）は
// 頂点バッファビュー側が持っている。だから POSITION だけを宣言しておけば、
//   Vertex   （position/normal/color = 40 バイト）
//   TexVertex（position/normal/uv    = 32 バイト）
// のどちらの頂点バッファでもそのまま描ける（先頭 12 バイトが position で共通なので）。
// おかげでシャドウ用 PSO は 1 つで済む。

cbuffer SceneConstants : register(b0)
{
    float4x4 gView;          // シャドウパスではライト視点のビュー行列が入る
    float4x4 gProj;          // 〃 ライトの正射影行列
    float4x4 gWorld;
    float4   gLightDir;
    float4   gLightColor;
    float4   gAmbient;
    float4   gCameraPos;
    float4x4 gLightViewProj; // 本描画パス用（このパスでは未使用）
    float4   gShadowParams;
};

struct VSInput
{
    float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_Position
{
    float4 wp = mul(float4(input.position, 1.0f), gWorld);
    return mul(mul(wp, gView), gProj);
}
