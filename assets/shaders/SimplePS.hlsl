#include "RayTracing.hlsli"
#ifndef ENGINE_SHADOWS
#define ENGINE_SHADOWS 1
#endif
// ピクセルシェーダー
// 環境光 + 平行光源による Blinn-Phong ライティング + シャドウマップを適用する。
//   最終色 = 環境光 + 影係数 ×（拡散(N·L) + 鏡面(N·H)）

cbuffer SceneConstants : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorld;
    float4   gLightDir;    // xyz: 平行光源が進む向き（正規化済み）
    float4   gLightColor;  // rgb: 色, w: 強度
    float4   gAmbient;     // rgb: 環境光
    float4   gCameraPos;   // xyz: カメラ位置, w: フォグ有効フラグ
    float4x4 gLightViewProj; // ステージL-03b: ライト視点のビュー×射影
    float4   gShadowParams;  // x=1/解像度, y=深度バイアス, z=有効フラグ, w=未使用
};

// ステージL-03b: シャドウマップ（ライト視点の深度）と比較サンプラー。
// SamplerComparisonState は「サンプルした深度と基準値を比較して 0/1 を返す」特殊な
// サンプラー。ハードウェアが 4 テクセル分の比較結果を線形補間してくれる（PCF の1段目）。
Texture2D              gShadowMap     : register(t1);
SamplerComparisonState gShadowSampler : register(s1);

struct PSInput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
};

// 影の係数を返す（1.0 = 完全に光が当たる / 0.0 = 完全に影）。
float ComputeShadow(float3 worldPos, float ndotl)
{
#if ENGINE_SHADOWS

    if (gShadowParams.z < 0.5f) return 1.0f;   // シャドウ無効時は素通し

    // ワールド座標をライト視点のクリップ空間へ。
    float4 lp = mul(float4(worldPos, 1.0f), gLightViewProj);
    lp.xyz /= lp.w;   // 正射影なので w は 1 だが、一般形で書いておく

    // クリップ空間(-1..1) → テクスチャ座標(0..1)。テクスチャの Y は下向きなので反転。
    float2 uv = lp.xy * float2(0.5f, -0.5f) + 0.5f;

    // シャドウマップの範囲外／遠クリップの外は「影なし」として扱う。
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f || lp.z > 1.0f) {
        return 1.0f;
    }

    // 深度バイアス。面がライトに対して傾いているほど、シャドウマップ 1 テクセルが
    // カバーする深度差が大きくなり自己遮蔽（シャドウアクネ＝縞模様）が出る。
    // そこで N·L が小さい（＝斜め）ほどバイアスを増やす。
    float bias     = gShadowParams.y * (1.0f + 2.0f * (1.0f - ndotl));
    float refDepth = lp.z - bias;

    // PCF 3×3。周囲 9 点の比較結果を平均して影の輪郭をぼかす（ジャギー対策）。
    float texel = gShadowParams.x;
    float sum   = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            sum += gShadowMap.SampleCmpLevelZero(
                gShadowSampler, uv + float2(x, y) * texel, refDepth);
        }
    }
    return sum / 9.0f;

#else
    return 1.0f;
#endif
}

float4 PSMain(PSInput input) : SV_Target
{
    float3 base = input.color.rgb;

    float3 N = normalize(input.normal);
    // gLightDir は「光が進む向き」。光源へ向かうベクトル L は符号反転。
    float3 L = normalize(-gLightDir.xyz);
    float3 V = normalize(gCameraPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float  diff = saturate(dot(N, L));
    // 表を向いている面だけ鏡面反射を出す（裏面のハイライトを防ぐ）
    float  spec = (diff > 0.0f) ? pow(saturate(dot(N, H)), 32.0f) : 0.0f;

    // ステージL-03b: 影の係数。環境光には掛けない（影の中も真っ暗にはならない）。
    float shadow = ComputeShadow(input.worldPos, diff);

#if ENGINE_RT_SHADOWS
    shadow = RayShadow(input.worldPos,N,L);
#endif
    float3 ambient  = gAmbient.rgb * base;
    float3 diffuse  = gLightColor.rgb * gLightColor.w * diff * base;
    float3 specular = gLightColor.rgb * gLightColor.w * spec * 0.3f;

    float3 color = ambient + shadow * (diffuse + specular);
#if ENGINE_RT_REFLECTIONS
    color = lerp(color,RayReflection(input.worldPos,N,V,L,gLightColor.rgb*gLightColor.w,gAmbient.rgb),.25f);
#endif

    // 距離フォグ（空気遠近）。gCameraPos.w=1 のとき有効／0（空など）で無効。
    // 遠くの物体を空の地平色へなじませて「奥行き」と空気感を出す。
    const float3 fogColor = float3(0.62f, 0.70f, 0.82f); // 空の地平色に合わせる
    const float  fogStart = 22.0f;
    const float  fogEnd   = 85.0f;
    float dist = length(gCameraPos.xyz - input.worldPos);
    float fog  = saturate((dist - fogStart) / (fogEnd - fogStart)) * gCameraPos.w;
    color = lerp(color, fogColor, fog);

    return float4(color, input.color.a);
}
