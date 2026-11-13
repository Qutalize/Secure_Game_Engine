#pragma once

#include <string>
#include <vector>

#include "engine/renderer/Renderer.h"  // Vertex 定義を使う

// Wavefront .obj ローダー（最小実装）。
// - v / vn / f に対応（vt は読み飛ばす）。
// - 面は多角形をファン三角形化。f の各頂点は v / v/vt / v//vn / v/vt/vn 形式に対応。
// - 法線が無い場合は面法線を計算する（フラットシェーディング）。
// - mtllib / usemtl を読み、.mtl の Kd（拡散色）を頂点色に反映する。
//
// ※ アセットは著作権フリー素材（Kenney/Quaternius 等）または自作のみを使うこと。
//    本プロジェクト同梱の assets/models/test_model.obj は自作（パブリックドメイン）。
namespace ObjLoader {

// path の .obj を読み込み、Renderer 用の頂点／インデックス配列を構築する。
// 実行ディレクトリの差を吸収するため "../" を前置した候補も探す。
// 成功で true。ファイルが開けない・頂点ゼロなら false（呼び出し側でフォールバック）。
bool Load(const std::string& path,
          std::vector<Vertex>& outVertices,
          std::vector<uint32_t>& outIndices);

} // namespace ObjLoader
