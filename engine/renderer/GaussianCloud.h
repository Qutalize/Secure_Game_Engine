#pragma once
//
// GaussianCloud — 3D Gaussian Splatting の点群コンテナ（ステージ3DGS / GS-01）。
//
// 位置づけ:
//   STAGE_3DGS.md の独立トラック。狙いは「絵をきれいにすること」ではなく
//   「深度バッファを使わない描画方式が既存アンチチートの前提を壊すか」を検証すること。
//   §7 のとおり結論は「特定の攻撃/防御を無効化するが、ウォールハック対策にはならない」。
//
// このヘッダは DX12 にも Renderer にも依存しない純粋なデータ構造。
//   → まだ CMake に配線しなくてもエンジンのビルドを壊さない。
//   → 描画（GS-02 以降）は別途 Renderer 側に足す。ここは「読めた点群を CPU で持つ」だけ。
//
// 参照実装: Kerbl et al., "3D Gaussian Splatting for Real-Time Radiance Field
//   Rendering", SIGGRAPH 2023 / graphdeco-inria/gaussian-splatting の .ply 形式。
//
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gs {

// SH の 0 次係数（DC 項）を線形 RGB に変換する定数。color = 0.5 + C0 * f_dc。
constexpr float kSH_C0 = 0.28209479177387814f;

// 1 個のガウシアン（デコード済みの値を持つ）。
//   .ply 生データは opacity=logit / scale=log / rot=非正規化クォータニオン なので、
//   PlyLoader が sigmoid / exp / 正規化 を適用した「そのまま使える」値をここに入れる。
struct GaussianSplat {
    float position[3] = { 0.0f, 0.0f, 0.0f };  // ワールド座標
    float scale[3]    = { 0.0f, 0.0f, 0.0f };  // 各軸の標準偏差（exp 済み・線形スケール）
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };  // 正規化クォータニオン (x, y, z, w)
    float color[3]    = { 0.5f, 0.5f, 0.5f };  // SH 0 次から求めた基本色（0..1 にクランプ）
    float opacity     = 1.0f;                  // sigmoid 済みの不透明度 α（0..1）

    // GS-05（攻撃実験）で外部から書き換える対象になるのは主に opacity。
    // §7.2 の A2「不透明度の改変で深度を触らず壁を透かす」がここに対応する。
};

// SH 高次係数（視点依存の色）。GS-02 では 0 次のみ使うので当面は未使用でよいが、
// 読めたものは捨てずに持っておく（GS 後半で視点依存カラーを足すとき使う）。
//   1 ガウシアンあたり restCoeffs 個 × 3 チャンネル。degree 3 なら 15 係数 = 45 float。
struct SphericalHarmonics {
    int                restCoeffsPerChannel = 0;   // 0 / 3 / 8 / 15（SH degree 0..3）
    std::vector<float>  rest;                       // size = splatCount * restCoeffsPerChannel * 3
};

struct GaussianCloud {
    std::vector<GaussianSplat> splats;
    SphericalHarmonics         sh;

    float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
    float boundsMax[3] = { 0.0f, 0.0f, 0.0f };

    // 元 .ply が持っていた SH degree（0 = DC のみ）。
    int  shDegree = 0;
    bool valid    = false;

    size_t Count() const { return splats.size(); }

    // AABB を splats から再計算する（PlyLoader が最後に呼ぶ）。
    void RecomputeBounds();
};

}  // namespace gs
