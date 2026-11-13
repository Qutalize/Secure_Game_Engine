// オーバーレイ（2D HUD）用の頂点シェーダー。
// 頂点位置は既に NDC（-1..1）で与えられる前提で、変換せずそのまま出力する。
// 照準（クロスヘア）や将来の HUD に使う。NORMAL は使わないが、
// 入力レイアウト（POSITION/NORMAL/COLOR）と一致させるため受け取るだけ受け取る。

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;   // 未使用
    float4 color    : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position.xy, 0.0f, 1.0f); // NDC をそのまま
    o.color    = input.color;
    return o;
}
