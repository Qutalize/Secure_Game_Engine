// 3DGS .ply ローダーの単体検証ツール（ステージ3DGS / GS-01）。
//
// 目的:
//   gltf_test と同じ発想。描画（GS-02 以降）に繋ぐ前に「.ply が正しく読めているか」
//   だけを CLI で確認する。点数・バウンディング・不透明度や色の分布が妥当なら
//   ローダーは正しい。
//
// 使い方:
//   ply_test.exe path/to/point_cloud.ply
//   ply_test.exe path/to/point_cloud.ply --stride 20        （1/20 に間引き）
//   ply_test.exe path/to/point_cloud.ply --dump 5           （先頭 5 個をダンプ）
//
// 学習済み .ply の入手（ライセンス確認必須・assets/CREDITS.md に記録すること）:
//   graphdeco-inria/gaussian-splatting の公式データセット出力、または
//   各種 3DGS ビューアのサンプル。人・銃には使わない（STAGE_3DGS.md §3）。
//
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/core/ConsoleUtf8.h"
#include "engine/renderer/PlyLoader.h"

int main(int argc, char** argv) {
    EnableConsoleUtf8();

    if (argc < 2) {
        printf("使い方: ply_test <point_cloud.ply> [--stride N] [--dump N]\n");
        return 1;
    }
    const std::string path = argv[1];
    gs::PlyLoadOptions opt;
    int dumpCount = 0;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stride") == 0 && i + 1 < argc) opt.stride = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) dumpCount = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) opt.positionScale = (float)std::atof(argv[++i]);
    }

    gs::GaussianCloud cloud;
    std::string error;
    if (!gs::LoadGaussianPly(path, opt, cloud, &error)) {
        printf("[NG] 読み込み失敗: %s\n", error.c_str());
        return 2;
    }

    printf("[OK] %s\n", path.c_str());
    printf("  ガウシアン数 : %zu  (stride %d)\n", cloud.Count(), opt.stride);
    printf("  SH degree    : %d  (rest 係数 %d /ch)\n", cloud.shDegree, cloud.sh.restCoeffsPerChannel);
    printf("  バウンディング: min(%.3f, %.3f, %.3f)  max(%.3f, %.3f, %.3f)\n",
           cloud.boundsMin[0], cloud.boundsMin[1], cloud.boundsMin[2],
           cloud.boundsMax[0], cloud.boundsMax[1], cloud.boundsMax[2]);
    printf("  サイズ       : %.2f x %.2f x %.2f\n",
           cloud.boundsMax[0] - cloud.boundsMin[0],
           cloud.boundsMax[1] - cloud.boundsMin[1],
           cloud.boundsMax[2] - cloud.boundsMin[2]);

    // 分布の健全性チェック
    double opAcc = 0.0, scAcc = 0.0;
    float opMin = 1e9f, opMax = -1e9f;
    double col[3] = { 0, 0, 0 };
    int nanCount = 0;
    for (const auto& g : cloud.splats) {
        opAcc += g.opacity;
        opMin = std::min(opMin, g.opacity);
        opMax = std::max(opMax, g.opacity);
        scAcc += (g.scale[0] + g.scale[1] + g.scale[2]) / 3.0;
        for (int c = 0; c < 3; ++c) col[c] += g.color[c];
        if (std::isnan(g.position[0]) || std::isnan(g.opacity) || std::isnan(g.scale[0])) ++nanCount;
    }
    const double n = (double)cloud.Count();
    printf("  不透明度 α   : 平均 %.3f  範囲 [%.3f, %.3f]  (sigmoid 済み＝0..1 に収まるはず)\n",
           opAcc / n, opMin, opMax);
    printf("  平均スケール : %.4f  (exp 済み・ワールド単位)\n", scAcc / n);
    printf("  平均色 RGB   : (%.2f, %.2f, %.2f)  (SH0 → 0..1)\n",
           col[0] / n, col[1] / n, col[2] / n);
    printf("  NaN を含む点 : %d\n", nanCount);

    bool sane = (opMin >= -0.001f && opMax <= 1.001f && nanCount == 0 && cloud.Count() > 0);
    printf("  判定         : %s\n", sane ? "[PASS]" : "[WARN] 値が想定範囲外");

    for (int i = 0; i < dumpCount && i < (int)cloud.Count(); ++i) {
        const auto& g = cloud.splats[i];
        printf("  [%d] pos(%.3f, %.3f, %.3f) scale(%.4f, %.4f, %.4f) "
               "rot(%.3f, %.3f, %.3f, %.3f) rgb(%.2f, %.2f, %.2f) a=%.3f\n",
               i, g.position[0], g.position[1], g.position[2],
               g.scale[0], g.scale[1], g.scale[2],
               g.rotation[0], g.rotation[1], g.rotation[2], g.rotation[3],
               g.color[0], g.color[1], g.color[2], g.opacity);
    }
    return sane ? 0 : 3;
}
