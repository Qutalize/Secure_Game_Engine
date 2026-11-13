#pragma once
//
// PNG デコーダ（ステージL-01b）。依存なしの自作実装。
//
// なぜ自作するか:
//   glTF のテクスチャは PNG か JPEG で埋め込まれる。stb_image を持ち込めば済むが、
//   本プロジェクトは「アロケータ・.pak・UDP・JSON を自作する」方針で来ているので、
//   ここも自作する。PNG は「zlib(deflate) 圧縮 + 行ごとのフィルタ」というだけの
//   素直な形式で、圧縮側を作らず展開側（inflate）だけなら現実的な分量に収まる。
//
// 対応範囲:
//   ○ ビット深度 8（カラータイプ 0=灰 / 2=RGB / 3=パレット / 4=灰+α / 6=RGBA）
//   ○ ビット深度 1/2/4（パレットのみ）
//   ○ ビット深度 16（上位バイトを採用して 8bit に落とす）
//   ○ tRNS（パレット/灰/RGB の透過指定）
//   × インターレース（Adam7）… 明示的に失敗させる。稀なうえ黙って壊れると原因が分かりにくい
//   × JPEG … glTF が JPEG を使っていたら PNG へ変換してもらう
//
#include <cstdint>
#include <string>
#include <vector>

// 展開結果は常に RGBA8（1 ピクセル 4 バイト・行優先）。
// Renderer::CreateTexture がこの形式を要求するのでそれに合わせてある。
struct PngImage {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4
    bool                 valid  = false;
};

// メモリ上の PNG を展開する。失敗したら false を返し error にメッセージを入れる。
bool DecodePng(const uint8_t* data, size_t size, PngImage& out, std::string* error);

// zlib ストリーム（PNG の IDAT や、将来の .pak 圧縮に使える）を展開する。
// PNG デコーダの内部で使うが、単体でも役に立つので公開している。
bool ZlibInflate(const uint8_t* data, size_t size,
                 std::vector<uint8_t>& out, std::string* error);
