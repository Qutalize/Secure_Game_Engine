// テクスチャ付き頂点シェーダー（PROMPT-G-01）。
// POSITION(float3) + NORMAL(float3) + TEXCOORD(float2) を入力に取り、
// world/view/proj で変換。ライティング用にワールド法線／座標も渡す。
// 既存の SimpleVS（頂点色）とは別レイアウト（COLOR ではなく TEXCOORD）。

cbuffer SceneConstants : register(b0)
{
    // CPU 側で転置してアップロードしているので mul(vector, matrix) 順で乗算する。
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorld;
    float4   gLightDir;    // xyz: 平行光源が進む向き（正規化済み）
    float4   gLightColor;  // rgb: 色, w: 強度
    float4   gAmbient;     // rgb: 環境光
    float4   gCameraPos;   // xyz: カメラ位置, w: フォグ有効フラグ
    // ステージL-03b（シャドウマップ）で追加。C++ 側 SceneConstants とメンバ順を一致させること。
    float4x4 gLightViewProj; // ライト視点のビュー×射影
    float4   gShadowParams;  // x=1/シャドウマップ解像度, y=深度バイアス, z=有効フラグ
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION;   // スペキュラ用ワールド座標
    float3 normal   : NORMAL;     // ワールド空間の法線
    float2 uv       : TEXCOORD;   // テクスチャ座標
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 wp = mul(float4(input.position, 1.0f), gWorld);
    o.worldPos = wp.xyz;
    o.position = mul(mul(wp, gView), gProj);
    // 回転＋一様スケール＋平行移動を想定（非一様スケールは未対応）。
    o.normal = normalize(mul(input.normal, (float3x3)gWorld));
    o.uv = input.uv;
    return o;
}
