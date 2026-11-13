// glTF ローダーの単体検証ツール（ステージL-01a）。
//
// 目的:
//   モデルの読み込みは「表示されない」ときに原因が
//   ①ローダーが壊れている ②描画側の配置がおかしい のどちらか分からなくなりがち。
//   そこで描画に繋ぐ前に、ローダーだけを CLI で確認できるようにした。
//
// 使い方:
//   gltf_test.exe assets/models/test_figure.glb
//   gltf_test.exe assets/models/test_figure.glb -v   （頂点を数個ダンプ）
//
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/core/ConsoleUtf8.h"
#include "engine/renderer/Animation.h"
#include "engine/renderer/GltfLoader.h"

int main(int argc, char** argv) {
    EnableConsoleUtf8();

    if (argc < 2) {
        printf("使い方: gltf_test <model.glb|model.gltf> [-v]\n");
        return 1;
    }
    const std::string path = argv[1];
    const bool verbose = (argc >= 3 && std::string(argv[2]) == "-v");

    GltfModel model;
    std::string error;
    if (!LoadGltf(path, model, &error)) {
        printf("[NG] 読み込み失敗: %s\n", error.c_str());
        return 2;
    }

    printf("[OK] %s\n", path.c_str());
    printf("  primitive 数 : %zu\n", model.primitives.size());
    printf("  頂点合計     : %zu\n", model.totalVertices);
    printf("  インデックス : %zu  (三角形 %zu 個)\n",
           model.totalIndices, model.totalIndices / 3);
    printf("  バウンディング: min(%.3f, %.3f, %.3f)  max(%.3f, %.3f, %.3f)\n",
           model.boundsMin.x, model.boundsMin.y, model.boundsMin.z,
           model.boundsMax.x, model.boundsMax.y, model.boundsMax.z);
    printf("  サイズ       : %.3f x %.3f x %.3f\n",
           model.boundsMax.x - model.boundsMin.x,
           model.boundsMax.y - model.boundsMin.y,
           model.boundsMax.z - model.boundsMin.z);

    printf("  画像数       : %zu\n", model.images.size());
    for (size_t i = 0; i < model.images.size(); ++i) {
        const GltfImage& im = model.images[i];
        if (im.valid) {
            // 平均色を出す。デコードが壊れていると真っ黒／真っ白／極端な値になるので、
            // 目視できない CLI でも「それらしく展開できたか」の当たりが付く。
            unsigned long long r = 0, g = 0, b = 0;
            const size_t n = static_cast<size_t>(im.width) * im.height;
            for (size_t k = 0; k < n; ++k) {
                r += im.rgba[k * 4 + 0]; g += im.rgba[k * 4 + 1]; b += im.rgba[k * 4 + 2];
            }
            printf("  image[%zu]     : %ux%u OK  平均色 RGB(%llu, %llu, %llu)\n",
                   i, im.width, im.height, r / n, g / n, b / n);
        } else {
            printf("  image[%zu]     : デコード失敗（JPEG など未対応形式の可能性）\n", i);
        }
    }

    for (size_t i = 0; i < model.primitives.size(); ++i) {
        const GltfPrimitive& p = model.primitives[i];
        printf("  [%zu] 頂点 %5zu / 索引 %5zu / material %2d / UV %s / 画像 %2d / "
               "色(%.2f, %.2f, %.2f, %.2f)\n",
               i, p.vertices.size(), p.indices.size(), p.materialIndex,
               p.hasUv ? "有" : "無", p.baseColorImage,
               p.baseColor.x, p.baseColor.y, p.baseColor.z, p.baseColor.w);
    }

    // 健全性チェック: インデックスが頂点数の範囲に収まっているか。
    // ここが壊れていると描画時に GPU 側で範囲外アクセスになり、原因が分かりにくい。
    size_t badIndices = 0;
    size_t badNormals = 0;
    for (const GltfPrimitive& p : model.primitives) {
        for (uint32_t idx : p.indices) {
            if (idx >= p.vertices.size()) ++badIndices;
        }
        for (const TexVertex& v : p.vertices) {
            const float len2 = v.normal.x * v.normal.x + v.normal.y * v.normal.y +
                               v.normal.z * v.normal.z;
            if (len2 < 0.9f || len2 > 1.1f) ++badNormals; // 正規化されていない法線
        }
        if (p.indices.size() % 3 != 0) {
            printf("  [警告] インデックス数が 3 の倍数ではありません\n");
        }
    }
    printf("  範囲外インデックス: %zu\n", badIndices);
    printf("  非正規化の法線    : %zu\n", badNormals);

    if (verbose && !model.primitives.empty()) {
        const GltfPrimitive& p = model.primitives[0];
        const size_t n = (p.vertices.size() < 6) ? p.vertices.size() : 6;
        printf("  --- primitive[0] の先頭 %zu 頂点 ---\n", n);
        for (size_t i = 0; i < n; ++i) {
            const TexVertex& v = p.vertices[i];
            printf("    pos(%7.3f,%7.3f,%7.3f) nrm(%6.3f,%6.3f,%6.3f) uv(%.3f,%.3f)\n",
                   v.position.x, v.position.y, v.position.z,
                   v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y);
        }
    }

    // ---- ステージL-04a: スキンとアニメーションの検証 ----
    printf("  ---- スキン / アニメーション ----\n");
    printf("  ノード       : %zu\n", model.nodes.size());
    printf("  スキン       : %zu\n", model.skins.size());
    for (size_t i = 0; i < model.skins.size(); ++i) {
        printf("    skin[%zu] '%s' : ボーン %zu 本\n", i, model.skins[i].name.c_str(),
               model.skins[i].jointNodes.size());
    }
    printf("  アニメーション: %zu クリップ\n", model.animations.size());
    for (size_t i = 0; i < model.animations.size(); ++i) {
        const GltfAnimation& a = model.animations[i];
        printf("    [%2zu] %-34s %6.2f 秒 / チャンネル %3zu / サンプラー %3zu\n",
               i, a.name.c_str(), a.duration, a.channels.size(), a.samplers.size());
    }

    size_t skinnedPrims = 0;
    for (const GltfPrimitive& p : model.primitives) {
        if (p.skinned) ++skinnedPrims;
    }
    printf("  スキン付き primitive: %zu / %zu\n", skinnedPrims, model.primitives.size());

    bool animOk = true;
    if (!model.skins.empty() && !model.animations.empty()) {
        // ★実際にサンプリングして「ボーンが動いているか」を数値で確かめる★
        // 描画に繋ぐ前にここで検証できるのが CLI ツールの価値。
        // クリップの先頭・中間・末尾でジョイント行列を取り、
        //   ・行列が有限か（NaN/Inf が無いか）
        //   ・時刻によって値が変わっているか（＝アニメーションが効いているか）
        // を見る。
        const int clip = 0;
        const float dur = model.animations[0].duration;
        std::vector<DirectX::XMFLOAT4X4> j0, j1, j2;
        AnimationPlayer::SampleAt(model, 0, clip, 0.0f, j0);
        AnimationPlayer::SampleAt(model, 0, clip, dur * 0.5f, j1);
        AnimationPlayer::SampleAt(model, 0, clip, dur, j2);

        size_t nonFinite = 0, moved = 0;
        float maxDelta = 0.0f;
        for (size_t k = 0; k < j0.size(); ++k) {
            const float* a = &j0[k]._11;
            const float* b = &j1[k]._11;
            const float* c = (k < j2.size()) ? &j2[k]._11 : b;
            float d = 0.0f;
            for (int e = 0; e < 16; ++e) {
                if (!std::isfinite(a[e]) || !std::isfinite(b[e]) || !std::isfinite(c[e])) {
                    ++nonFinite;
                }
                d = (std::max)(d, std::fabs(a[e] - b[e]));
            }
            if (d > 1e-4f) ++moved;
            maxDelta = (std::max)(maxDelta, d);
        }
        printf("  クリップ[0] '%s' を t=0 / %.2f / %.2f で評価:\n",
               model.animations[0].name.c_str(), dur * 0.5f, dur);
        printf("    ジョイント行列 %zu 個 / 動いたボーン %zu 本 / 最大変化 %.4f / 非有限値 %zu\n",
               j0.size(), moved, maxDelta, nonFinite);

        // ★バインドポーズの検算★
        // clip=-1（アニメーション無し）でサンプリングすると、ジョイント行列は
        // 「逆バインド行列 × バインドポーズの世界変換」＝ほぼ単位行列になるはず。
        // ここが単位行列から大きく外れていたら、逆バインド行列の読み込みか
        // 合成順のどちらかが間違っている（描画すると全身が吹き飛ぶ）。
        std::vector<DirectX::XMFLOAT4X4> bind;
        AnimationPlayer::SampleAt(model, 0, -1, 0.0f, bind);
        float bindErr = 0.0f;
        for (const DirectX::XMFLOAT4X4& m : bind) {
            const float* v = &m._11;
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    const float expect = (r == c) ? 1.0f : 0.0f;
                    bindErr = (std::max)(bindErr, std::fabs(v[r * 4 + c] - expect));
                }
            }
        }
        printf("    バインドポーズの単位行列からのズレ: %.5f %s\n", bindErr,
               (bindErr < 0.01f) ? "（正常）"
                                 : "（★異常★ 逆バインド行列か合成順を疑ってください）");
        if (bindErr >= 0.01f) animOk = false;

        if (nonFinite > 0) {
            printf("    [警告] 非有限値が含まれています（逆バインド行列か補間の不具合）\n");
            animOk = false;
        }
        if (moved == 0) {
            printf("    [警告] どのボーンも動いていません（チャンネルの結線を確認）\n");
            animOk = false;
        }
        if (skinnedPrims == 0) {
            printf("    [警告] スキン付き primitive がありません（JOINTS_0/WEIGHTS_0 未取得）\n");
            animOk = false;
        }
        if (animOk) {
            printf("    → アニメーションのサンプリングは正常です\n");
        }
    } else if (!model.skins.empty()) {
        printf("  （アニメーション無し。バインドポーズのみ）\n");
    }

    const bool healthy = (badIndices == 0 && badNormals == 0 && animOk);
    printf("%s\n", healthy ? "[PASS] 健全性チェックを通過しました"
                           : "[FAIL] 健全性チェックに問題があります");
    return healthy ? 0 : 3;
}
