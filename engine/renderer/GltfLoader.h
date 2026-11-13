#pragma once
//
// glTF 2.0 ローダー（ステージL-01a）。
//
// なぜ .obj から乗り換えるか:
//   .obj は「頂点・法線・UV・面」しか持てない。マテリアルは別ファイル(.mtl)で表現力が乏しく、
//   ボーン／スキニング／アニメーションの概念が無い。L-04（スキンドアニメ）まで見据えると
//   .obj では行き止まりになる。glTF 2.0 は PBR マテリアル・スキン・アニメーションを
//   標準で持つ「3D 界の JPEG」的な位置づけで、CC0 アセットの入手性も高い。
//
// 対応範囲（L-01b 時点）:
//   ○ .glb（バイナリ単一ファイル・推奨）
//   ○ .gltf（JSON）+ 外部 .bin / base64 data URI
//   ○ ノード階層の TRS / matrix を合成して頂点をモデル空間へ焼き込む
//   ○ POSITION / NORMAL / TEXCOORD_0 / インデックス（各種 componentType）
//   ○ マテリアルの baseColorFactor（単色）
//   ○ テクスチャ画像（PNG）のデコード ※L-01b で追加。JPEG は未対応
//   × スキン／アニメーション        → L-04 で対応
//   × スパースアクセサ・拡張        → 未対応（使われていたら警告して無視）
//
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "engine/renderer/Renderer.h"  // Vertex / TexVertex

// glTF の 1 プリミティブ（= 1 つのマテリアルで描かれるメッシュの塊）。
// glTF では 1 つの mesh が複数の primitive を持てるので、これが描画の最小単位になる。
struct GltfPrimitive {
    // 頂点はノード階層の変換を適用済み（モデル空間）。呼び出し側は配置行列だけ与えればよい。
    std::vector<TexVertex> vertices;
    std::vector<uint32_t>  indices;

    // ステージL-02: 接線。法線マップに必須。glTF が TANGENT 属性を持たない場合は
    // ローダーが UV から生成する。w は従法線の符号。
    std::vector<DirectX::XMFLOAT4> tangents;

    // ---- ステージL-04a: スキニング ----
    // JOINTS_0（影響するボーン 4 本の番号）と WEIGHTS_0（その重み）。
    // skinned=false のときは空。
    std::vector<DirectX::XMUINT4> joints;
    std::vector<DirectX::XMFLOAT4> weights;
    bool skinned  = false;
    int  skinIndex = -1;   // このプリミティブを描くノードが参照する skin

    DirectX::XMFLOAT4 baseColor{ 1.0f, 1.0f, 1.0f, 1.0f }; // マテリアルの基本色
    int  materialIndex   = -1;
    int  baseColorImage  = -1;   // glTF の images[] のインデックス（L-01b）
    // ---- ステージL-02（PBR）----
    int   normalImage    = -1;   // 法線マップ
    int   ormImage       = -1;   // metallicRoughnessTexture（G=粗さ / B=金属度）
    float metallicFactor  = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale     = 1.0f;
    bool hasUv           = false;
};

// glTF に埋め込まれた／参照された画像を RGBA8 に展開したもの（ステージL-01b）。
// GltfPrimitive::baseColorImage がこの配列のインデックスを指す。
struct GltfImage {
    uint32_t             width  = 0;
    uint32_t             height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4
    bool                 valid  = false;  // デコード失敗時は false（JPEG など）
};

// ---------------------------------------------------------------------------
// ステージL-04a: スキン（ボーン）とアニメーション
// ---------------------------------------------------------------------------

// ノード階層。アニメーションは「ノードの TRS を時間で書き換える」ものなので、
// 再生時に階層をもう一度合成する必要がある＝読み込み時に木を保持しておく。
struct GltfNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    // ローカル TRS（アニメーションはこの 3 つを上書きする）
    DirectX::XMFLOAT3 translation{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 rotation{ 0.0f, 0.0f, 0.0f, 1.0f };  // クォータニオン xyzw
    DirectX::XMFLOAT3 scale{ 1.0f, 1.0f, 1.0f };
    bool hasMatrix = false;                 // matrix 指定のノード（TRS を持たない）
    DirectX::XMFLOAT4X4 matrix{};           // hasMatrix のとき有効（DirectXMath 規約）
    int mesh = -1;
    int skin = -1;
};

// スキン = 「どのノードがボーンか」＋「各ボーンの逆バインド行列」。
// 逆バインド行列は「バインドポーズでのボーンの世界変換の逆」で、
// 頂点をボーンのローカル空間へ引き戻すために使う。
struct GltfSkin {
    std::string name;
    std::vector<int> jointNodes;                    // joints[i] = ノード番号
    std::vector<DirectX::XMFLOAT4X4> inverseBind;   // DirectXMath 規約（転置済み）
};

// アニメーションのサンプラー（キーフレーム列）。
struct GltfAnimSampler {
    enum class Interp { Linear, Step, CubicSpline };
    std::vector<float> times;    // キー時刻（秒）
    std::vector<float> values;   // 値。1 キーあたり comps 個（CubicSpline は 3 倍）
    int    comps  = 3;           // 3 = 平行移動/スケール, 4 = 回転（クォータニオン）
    Interp interp = Interp::Linear;
};

// どのノードのどのプロパティを、どのサンプラーで動かすか。
struct GltfAnimChannel {
    enum class Path { Translation, Rotation, Scale, Unsupported };
    int  node    = -1;
    Path path    = Path::Unsupported;
    int  sampler = -1;
};

struct GltfAnimation {
    std::string name;
    float duration = 0.0f;   // 全チャンネルの最終キー時刻
    std::vector<GltfAnimSampler> samplers;
    std::vector<GltfAnimChannel> channels;
};

struct GltfModel {
    std::vector<GltfPrimitive> primitives;
    std::vector<GltfImage>     images;   // ステージL-01b
    // ---- ステージL-04a ----
    std::vector<GltfNode>      nodes;
    std::vector<GltfSkin>      skins;
    std::vector<GltfAnimation> animations;
    std::vector<int>           rootNodes;   // シーンのルートノード番号
    DirectX::XMFLOAT3 boundsMin{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 boundsMax{ 0.0f, 0.0f, 0.0f };
    size_t totalVertices = 0;
    size_t totalIndices  = 0;
    bool   valid         = false;
};

// .glb / .gltf を読み込む。失敗したら false を返し error にメッセージを入れる。
// path は実行時の作業ディレクトリからの相対パス、または絶対パス。
bool LoadGltf(const std::string& path, GltfModel& out, std::string* error);

// GltfPrimitive を「頂点色つき頂点（Vertex）」へ変換する。
// テクスチャを持たない primitive 用。マテリアルの baseColor を頂点色に焼いて
// Lit マテリアル経路（DrawMesh）で描く。
std::vector<Vertex> MakeLitVertices(const GltfPrimitive& prim);

// GltfPrimitive を PBR 用頂点（位置/法線/接線/UV）へ変換する（ステージL-02）。
// tangents が空なら (1,0,0,1) で埋める（法線マップが無ければ実害は無い）。
std::vector<PbrVertex> MakePbrVertices(const GltfPrimitive& prim);

// GltfPrimitive を「スキン付き PBR 頂点」へ変換する（ステージL-04b）。
// prim.skinned が false のときも呼べるが、その場合 joints は全て 0、
// weights は (1,0,0,0) になる（＝常にボーン 0 の変換を 100% 適用）。
// ボーン 0 が単位行列でなければ意図しない変形になるので、実際には
// 呼び出し側が prim.skinned を見て CreateSkinnedPbrMesh / CreatePbrMesh を使い分けること。
std::vector<SkinnedPbrVertex> MakeSkinnedPbrVertices(const GltfPrimitive& prim);
