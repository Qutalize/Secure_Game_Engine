#pragma once
//
// PlyLoader — 3D Gaussian Splatting の学習済み .ply を読む（ステージ3DGS / GS-01）。
//
// なぜ自作するか:
//   本プロジェクトの方針（アロケータ・.pak・UDP・JSON・PNG・glTF を自作）に合わせる。
//   3DGS の .ply は「ASCII ヘッダ + binary_little_endian の固定長頂点レコード」という
//   素直な形式なので、汎用 PLY パーサとして現実的な分量に収まる。
//
// 対応範囲:
//   ○ format ascii / binary_little_endian / binary_big_endian 1.0
//   ○ element vertex のみ解釈（他の element はスキップ）
//   ○ property <scalar type> <name>（char/uchar/short/ushort/int/uint/float/double、
//      および list 型は「読み飛ばし」対応＝Gaussian .ply には通常出てこない）
//   ○ 3DGS 標準プロパティ:
//        x y z / nx ny nz / f_dc_0..2 / f_rest_0..N / opacity / scale_0..2 / rot_0..3
//   ○ opacity=sigmoid, scale=exp, rot=正規化 を適用してデコード
//   ○ SH degree の自動判定（f_rest_* の個数から 0/1/2/3）
//   × テクスチャ・面（3DGS の .ply に面は無い）
//
// 依存: 標準ライブラリのみ（DX12 / Renderer に一切依存しない）。
//
#include <cstdint>
#include <string>

#include "engine/renderer/GaussianCloud.h"

namespace gs {

struct PlyLoadOptions {
    // 点数を間引く（1 = 全部、10 = 1/10）。速度は目的でないので巨大 .ply は間引いて実験する。
    int  stride = 1;
    // 読み込み時に位置へ掛けるスケール（3DGS のシーンは単位がまちまちなので調整用）。
    float positionScale = 1.0f;
};

// .ply を読み込む。失敗したら false を返し error にメッセージを入れる。
// path は作業ディレクトリからの相対 or 絶対パス。
bool LoadGaussianPly(const std::string& path,
                     const PlyLoadOptions& options,
                     GaussianCloud& out,
                     std::string* error);

}  // namespace gs
