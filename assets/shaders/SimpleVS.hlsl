// 頂点シェーダー
// POSITION(float3) + NORMAL(float3) + COLOR(float4) を入力に取り、
// world/view/proj 行列で変換してクリップ座標を出力する。
// ライティング用に「ワールド空間の法線」と「ワールド座標」も出力する。

cbuffer SceneConstants : register(b0)
{
    // CPU 側で転置（transpose）してアップロードしているため、
    // ここでは mul(vector, matrix) の順で乗算する。
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorld;
    float4   gLightDir;    // xyz: 平行光源が進む向き（正規化済み）
    float4   gLightColor;  // rgb: 色, w: 強度
    float4   gAmbient;     // rgb: 環境光
    float4   gCameraPos;   // xyz: カメラ位置, w: フォグ有効フラグ
    // ステージL-03b（シャドウマップ）で追加。C++ 側 SceneConstants とメンバ順を一致させること。
    float4x4 gLightViewProj; // ライト視点のビュー×射影（影の判定に使う）
    float4   gShadowParams;  // x=1/シャドウマップ解像度, y=深度バイアス, z=有効フラグ, w=未使用
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION;   // スペキュラ／影の判定用のワールド座標
    float3 normal   : NORMAL;     // ワールド空間の法線
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;

    // ワールド座標
    float4 wp = mul(float4(input.position, 1.0f), gWorld);
    o.worldPos = wp.xyz;

    // クリップ座標
    o.position = mul(mul(wp, gView), gProj);

    // 法線をワールドへ。回転＋一様スケール＋平行移動なら (float3x3)gWorld で十分。
    // ※ 非一様スケールを使う場合は world の逆転置行列が必要（このプロジェクトでは未使用）。
    o.normal = normalize(mul(input.normal, (float3x3)gWorld));

    o.color = input.color;
    return o;
}
