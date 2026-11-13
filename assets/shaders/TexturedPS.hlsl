// テクスチャ付きピクセルシェーダー（PROMPT-G-01）。
// テクスチャ(t0) を静的サンプラー(s0) でサンプルし、SimplePS と同じ
// 環境光＋平行光源(Blinn-Phong) を掛けて陰影を付ける。
//   最終色 = テクスチャ色 ×（環境光 + 拡散 N·L）+ 鏡面(N·H)

cbuffer SceneConstants : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorld;
    float4   gLightDir;
    float4   gLightColor;
    float4   gAmbient;
    float4   gCameraPos;
    float4x4 gLightViewProj; // ステージL-03b: ライト視点のビュー×射影
    float4   gShadowParams;  // x=1/解像度, y=深度バイアス, z=有効フラグ
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

// ステージL-03b: シャドウマップ（ライト視点の深度）と比較サンプラー。SimplePS と同じ。
Texture2D              gShadowMap     : register(t1);
SamplerComparisonState gShadowSampler : register(s1);

// 影の係数を返す（1.0 = 光が当たる / 0.0 = 影）。SimplePS.hlsl と同じ内容。
// ※ HLSL の #include は実行時フォールバックコンパイル（D3DCompile）でインクルード
//   ハンドラが要るため、あえて共通化せず両方に同じ関数を置いている。
float ComputeShadow(float3 worldPos, float ndotl)
{
    if (gShadowParams.z < 0.5f) return 1.0f;

    float4 lp = mul(float4(worldPos, 1.0f), gLightViewProj);
    lp.xyz /= lp.w;

    float2 uv = lp.xy * float2(0.5f, -0.5f) + 0.5f;
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || lp.z > 1.0f) {
        return 1.0f;
    }

    float bias     = gShadowParams.y * (1.0f + 2.0f * (1.0f - ndotl));
    float refDepth = lp.z - bias;

    float texel = gShadowParams.x;
    float sum   = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            sum += gShadowMap.SampleCmpLevelZero(
                gShadowSampler, uv + float2(x, y) * texel, refDepth);
        }
    }
    return sum / 9.0f;
}

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD;
};

float4 PSMain(PSInput input) : SV_Target
{
    float4 tex  = gTexture.Sample(gSampler, input.uv);
    float3 base = tex.rgb;

    float3 N = normalize(input.normal);
    float3 L = normalize(-gLightDir.xyz);          // 光源へ向かうベクトル
    float3 V = normalize(gCameraPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float diff = saturate(dot(N, L));
    float spec = (diff > 0.0f) ? pow(saturate(dot(N, H)), 32.0f) : 0.0f;

    // ステージL-03b: 影の係数。環境光には掛けない。
    float shadow = ComputeShadow(input.worldPos, diff);

    float3 ambient  = gAmbient.rgb * base;
    float3 diffuse  = gLightColor.rgb * gLightColor.w * diff * base;
    float3 specular = gLightColor.rgb * gLightColor.w * spec * 0.15f;

    float3 color = ambient + shadow * (diffuse + specular);

    // 距離フォグ（SimplePS と同じ。gCameraPos.w=1 で有効）。テクスチャ床も遠方でなじませる。
    const float3 fogColor = float3(0.62f, 0.70f, 0.82f);
    const float  fogStart = 22.0f;
    const float  fogEnd   = 85.0f;
    float dist = length(gCameraPos.xyz - input.worldPos);
    float fog  = saturate((dist - fogStart) / (fogEnd - fogStart)) * gCameraPos.w;
    color = lerp(color, fogColor, fog);

    return float4(color, tex.a);
}
