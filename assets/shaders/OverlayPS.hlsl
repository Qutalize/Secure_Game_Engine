// オーバーレイ（2D HUD）用のピクセルシェーダー。
// ライティングを一切かけず、頂点色をそのまま出力する（常に一定の明るさ）。

struct PSInput
{
    float4 position : SV_Position;
    float4 color    : COLOR;
};

float4 PSMain(PSInput input) : SV_Target
{
    return input.color;
}
