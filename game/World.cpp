#include "game/World.h"

#include <algorithm>  // AddPerimeter の std::max
#include <cmath>      // AddModule の cosf/sinf/fabsf（回転後 AABB の算出）
#include <utility>
#include <vector>

#include "engine/core/Logger.h"
#include "engine/renderer/ObjLoader.h"
#include "engine/renderer/GltfLoader.h"   // ステージL-01a
#include "game/GroundTexture.h"   // 地面/壁/木箱テクスチャの手続き生成（PROMPT-I-02 / 視覚増分2）

using namespace DirectX;

namespace {

// 単色の直方体メッシュ（中心原点、半サイズ hx/hy/hz）を作る。
// ライティング対応のため面ごとに法線を持たせる必要があり、
// 8 頂点共有ではなく「面ごとに 4 頂点（計 24 頂点）」で構築する。
void MakeBox(float hx, float hy, float hz, const XMFLOAT4& color,
             std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    struct Face {
        XMFLOAT3 normal;
        XMFLOAT3 corner[4]; // 外向きに見て反時計回り
    };
    const Face faces[6] = {
        { {  0,  0, -1 }, { { -hx, -hy, -hz }, {  hx, -hy, -hz }, {  hx,  hy, -hz }, { -hx,  hy, -hz } } }, // -Z
        { {  0,  0,  1 }, { {  hx, -hy,  hz }, { -hx, -hy,  hz }, { -hx,  hy,  hz }, {  hx,  hy,  hz } } }, // +Z
        { { -1,  0,  0 }, { { -hx, -hy,  hz }, { -hx, -hy, -hz }, { -hx,  hy, -hz }, { -hx,  hy,  hz } } }, // -X
        { {  1,  0,  0 }, { {  hx, -hy, -hz }, {  hx, -hy,  hz }, {  hx,  hy,  hz }, {  hx,  hy, -hz } } }, // +X
        { {  0,  1,  0 }, { { -hx,  hy, -hz }, {  hx,  hy, -hz }, {  hx,  hy,  hz }, { -hx,  hy,  hz } } }, // +Y
        { {  0, -1,  0 }, { { -hx, -hy,  hz }, {  hx, -hy,  hz }, {  hx, -hy, -hz }, { -hx, -hy, -hz } } }, // -Y
    };
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        for (int i = 0; i < 4; ++i) {
            v.push_back(Vertex{ f.corner[i], f.normal, color });
        }
        // 2 三角形（カリングは無効なので巻き方向は描画に影響しない）
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
}

// グラデーションの空（巨大な箱）。全頂点の法線を上向きに揃えるので、ライティングが
// 面ごとにばらつかず、頂点色（高さで上=濃紺→下=明色）の縦グラデーションが素直に出る。
// 深度書き込みなしで最初に描くと背景として機能する。
Mesh MakeGradientSky(Renderer& renderer) {
    const float H = 180.0f;                       // 遠クリップ(500)内に収まる大きさ
    const XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
    const XMFLOAT3 topCol { 0.10f, 0.18f, 0.42f }; // 天頂（濃い青）
    const XMFLOAT3 botCol { 0.60f, 0.68f, 0.80f }; // 地平（明るい青灰）

    auto colAt = [&](float y) {
        const float t = (y + H) / (2.0f * H);      // 0=底, 1=天頂
        return XMFLOAT4{ botCol.x + (topCol.x - botCol.x) * t,
                         botCol.y + (topCol.y - botCol.y) * t,
                         botCol.z + (topCol.z - botCol.z) * t, 1.0f };
    };

    struct Face { XMFLOAT3 c[4]; };
    const Face faces[6] = {
        { { { -H, -H, -H }, {  H, -H, -H }, {  H,  H, -H }, { -H,  H, -H } } },
        { { {  H, -H,  H }, { -H, -H,  H }, { -H,  H,  H }, {  H,  H,  H } } },
        { { { -H, -H,  H }, { -H, -H, -H }, { -H,  H, -H }, { -H,  H,  H } } },
        { { {  H, -H, -H }, {  H, -H,  H }, {  H,  H,  H }, {  H,  H, -H } } },
        { { { -H,  H, -H }, {  H,  H, -H }, {  H,  H,  H }, { -H,  H,  H } } },
        { { { -H, -H,  H }, {  H, -H,  H }, {  H, -H, -H }, { -H, -H, -H } } },
    };
    std::vector<Vertex>   v;
    std::vector<uint32_t> idx;
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        for (int i = 0; i < 4; ++i) v.push_back(Vertex{ f.c[i], up, colAt(f.c[i].y) });
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
    return renderer.CreateMesh(v, idx);
}

// テクスチャ付きの直方体（実寸・法線付き）。面ごとに「その面の実寸 × tilesPerMeter」で
// UV をタイリングするので、長い壁でもレンガ/木目が引き伸ばされず自然に繰り返す（WRAP 前提）。
Mesh MakeTexBox(Renderer& renderer, float hx, float hy, float hz, float tpm) {
    struct Face {
        XMFLOAT3 normal;
        XMFLOAT3 corner[4]; // 外向きに反時計回り
        XMFLOAT2 uvsize;    // この面の (幅, 高さ) の実寸（UV スケールに使う）
    };
    const Face faces[6] = {
        { {  0,  0, -1 }, { { -hx, -hy, -hz }, {  hx, -hy, -hz }, {  hx,  hy, -hz }, { -hx,  hy, -hz } }, { 2*hx, 2*hy } }, // -Z
        { {  0,  0,  1 }, { {  hx, -hy,  hz }, { -hx, -hy,  hz }, { -hx,  hy,  hz }, {  hx,  hy,  hz } }, { 2*hx, 2*hy } }, // +Z
        { { -1,  0,  0 }, { { -hx, -hy,  hz }, { -hx, -hy, -hz }, { -hx,  hy, -hz }, { -hx,  hy,  hz } }, { 2*hz, 2*hy } }, // -X
        { {  1,  0,  0 }, { {  hx, -hy, -hz }, {  hx, -hy,  hz }, {  hx,  hy,  hz }, {  hx,  hy, -hz } }, { 2*hz, 2*hy } }, // +X
        { {  0,  1,  0 }, { { -hx,  hy, -hz }, {  hx,  hy, -hz }, {  hx,  hy,  hz }, { -hx,  hy,  hz } }, { 2*hx, 2*hz } }, // +Y
        { {  0, -1,  0 }, { { -hx, -hy,  hz }, {  hx, -hy,  hz }, {  hx, -hy, -hz }, { -hx, -hy, -hz } }, { 2*hx, 2*hz } }, // -Y
    };
    std::vector<TexVertex> v;
    std::vector<uint32_t>  idx;
    for (const Face& f : faces) {
        const uint32_t base = static_cast<uint32_t>(v.size());
        const float tu = f.uvsize.x * tpm;  // この面の横タイル数
        const float tv = f.uvsize.y * tpm;  // この面の縦タイル数
        const XMFLOAT2 uv[4] = { { 0, tv }, { tu, tv }, { tu, 0 }, { 0, 0 } };
        for (int i = 0; i < 4; ++i) v.push_back(TexVertex{ f.corner[i], f.normal, uv[i] });
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }
    return renderer.CreateTexturedMesh(v, idx);
}

// テクスチャ床（±half 四方の平面）。地面テクスチャを tiles 回タイリングする UV を張る。
Mesh MakeTexFloor(Renderer& renderer, float floorSize) {
    const float h = floorSize * 0.5f;
    const float tiles = floorSize / 5.0f;      // 1 タイル ≒ 5m
    const XMFLOAT3 up{ 0.0f, 1.0f, 0.0f };
    std::vector<TexVertex> v = {
        { { -h, 0.0f, -h }, up, { 0.0f,  0.0f  } },
        { {  h, 0.0f, -h }, up, { tiles, 0.0f  } },
        { {  h, 0.0f,  h }, up, { tiles, tiles } },
        { { -h, 0.0f,  h }, up, { 0.0f,  tiles } },
    };
    std::vector<uint32_t> idx = { 0, 2, 1, 0, 3, 2 };
    return renderer.CreateTexturedMesh(v, idx);
}

// --- 配置モジュールの仕様 ----------------------------------------------------
// Poly Universal Pack（Modular Parts / Survival / City / Nature）を glTF 化したもの。
//
// ★寸法と原点はここに書かない★ .glb 自身のバウンディングから読み取って、AddModule が
//   「水平中心合わせ＋接地」を自動で行う。理由は、アセットの原点が作者・用途ごとに
//   てんでばらばらだから:
//     建築モジュール : 原点が角     （min(0,0,0)〜max(w,h,d)。グリッドに敷き詰めやすい）
//     街の置物       : ほぼ XZ 中心 （ただし minY が -0.2 前後＝わずかに地面下）
//     植物           : 根が地面下まで伸びていて minY が -0.65〜-1.31 と大きく負
//   これを手書きの寸法表で吸収しようとすると、モジュールを足すたびに間違える。
//   ファイル自身が知っている情報を使うほうが堅い（実際、公園アセットの取り込みで
//   手書き表が破綻したのでこの方式へ切り替えた）。
//
// ここに残すのは「ファイルからは分からないこと」だけ:
struct ModuleSpec {
    const char* path;
    float colScaleXZ;   // 当たり判定の水平サイズ／見た目の水平サイズ。1.0=見た目どおり。
                        // 木のように「見た目は葉で大きいが、ぶつかるのは幹だけ」という
                        // ものを小さくするために使う。
    bool  solidDefault; // 既定で当たり判定を作るか（ブランコのように下をくぐれる
                        // 構造物は false にして、見えない壁で塞がないようにする）
};

// enum ArenaModule の並びと 1 対 1 で対応させること。
const ModuleSpec kModuleSpecs[] = {
    // --- FPS アリーナ（工業/軍事）---
    { "assets/models/external/SM_Wall_5x3m_Ext.glb",              1.00f, true  },
    { "assets/models/external/SM_Wall_Door_Regular_3x3m_Ext.glb", 1.00f, true  },
    { "assets/models/external/SM_Wall_Window_3x3m_Ext.glb",       1.00f, true  },
    { "assets/models/external/SM_Barrell_Plain_A_Closed.glb",     1.00f, true  },
    { "assets/models/external/SM_Container_City.glb",             1.00f, true  },
    // --- かくれんぼ（公園/幼稚園）---
    { "assets/models/external/SM_Fence_Brick_3m_A.glb",           1.00f, true  },
    { "assets/models/external/SM_Slide_Plastic_City.glb",         0.60f, true  }, // 台と階段ぶんだけ
    { "assets/models/external/SM_Swing_City.glb",                 1.00f, false }, // 下をくぐれる
    { "assets/models/external/SM_Horse_Playground_City.glb",      1.00f, true  },
    { "assets/models/external/SM_Bench_A_City.glb",               1.00f, true  },
    { "assets/models/external/SM_Table_A_City.glb",               1.00f, true  },
    { "assets/models/external/SM_Bush_Round_Big.glb",             0.60f, true  }, // 葉に少しめり込める
    { "assets/models/external/SM_Bush_Round_Medium.glb",          0.60f, true  },
    { "assets/models/external/SM_Tree_Birch_Mature_A.glb",        0.20f, true  }, // 幹だけ
};
static_assert(sizeof(kModuleSpecs) / sizeof(kModuleSpecs[0]) ==
                  static_cast<size_t>(ArenaModule::Count),
              "kModuleSpecs と enum ArenaModule の要素数を一致させること");

// 扉モジュールの開口部の半幅（メートル）。当たり判定をここだけ空けて通り抜けられるようにする。
constexpr float kDoorOpeningHalf = 0.7f;

} // namespace

void World::Build(Renderer& renderer, WorldLayout layout) {
    m_layout    = layout;
    m_floorSize = (layout == WorldLayout::Default) ? kFloorSize : 40.0f;

    // --- 床（m_floorSize 四方の平らなクアッド。単色の薄い板）---
    {
        const float h = m_floorSize * 0.5f;
        const XMFLOAT4 gray{ 0.35f, 0.35f, 0.40f, 1.0f };
        const XMFLOAT3 up{ 0.0f, 1.0f, 0.0f }; // 床の法線は真上
        std::vector<Vertex> v = {
            { { -h, 0.0f, -h }, up, gray },
            { {  h, 0.0f, -h }, up, gray },
            { {  h, 0.0f,  h }, up, gray },
            { { -h, 0.0f,  h }, up, gray },
        };
        std::vector<uint32_t> idx = { 0, 2, 1, 0, 3, 2 };
        m_floorMesh = renderer.CreateMesh(v, idx);
    }

    // --- 単位立方体メッシュ（half=1。Default の遮蔽物で使い回す）---
    {
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, XMFLOAT4{ 0.8f, 0.3f, 0.3f, 1.0f }, v, idx);
        m_cubeMesh = renderer.CreateMesh(v, idx);
    }

    // --- 外部モデル (.glb / glTF 2.0) を読み込む（ステージL-01a/L-04b）---
    //  ★レイアウトを問わず読み込む必要があるので、arena の早期 return より前で呼ぶ
    //   （FpsArena/HideArena は下の early return で BuildFpsArena/BuildHideArena に
    //   分岐して抜けてしまうため、そこより後に置くと arena では一切実行されない）。
    LoadGltfCharacter(renderer);

    // arena レイアウトは背景の空＋テクスチャ地面を持つ（Default は従来どおり）。
    if (layout != WorldLayout::Default) {
        m_skyMesh = MakeGradientSky(renderer);

        // 地面テクスチャ（FPS=コンクリート / かくれんぼ=草地）を生成してタイル床に貼る。
        std::vector<uint8_t> px;
        if (layout == WorldLayout::FpsArena) groundtex::RenderConcrete(256, 256, px);
        else                                 groundtex::RenderGrass(256, 256, px);
        m_floorTex     = renderer.CreateTexture(256, 256, px.data());
        m_floorTexMesh = MakeTexFloor(renderer, m_floorSize);
        m_floorTextured = m_floorTex.valid;
    }

    // 広い闘技場レイアウトは別関数で構築して早期リターン（既存互換を壊さない）。
    if (layout == WorldLayout::FpsArena) {
        BuildFpsArena(renderer);
        return;
    }
    if (layout == WorldLayout::HideArena) {
        BuildHideArena(renderer);
        return;
    }

    // --- 隠し直方体（シアン色、1.0 x 1.4 x 1.0）---
    //  起動時の視点（カメラ ~(0,1.7,-6) で +Z を向く）から見ると、
    //  赤い箱 (0,1,6) の真後ろ (0,0.7,9) にあり、通常は完全に隠れている。
    //  赤い箱より小さくして、シルエットからはみ出さないようにしている。
    {
        std::vector<Vertex> v;
        std::vector<uint32_t> idx;
        MakeBox(0.5f, 0.7f, 0.5f, XMFLOAT4{ 0.1f, 0.9f, 0.9f, 1.0f }, v, idx);
        m_hiddenMesh = renderer.CreateMesh(v, idx);
    }

    // --- 遮蔽物 5 個を配置（各 2x2x2、中心 Y=1 で床に接地）---
    const XMFLOAT3 centers[5] = {
        {  4.0f, 1.0f,  4.0f },
        { -4.0f, 1.0f,  4.0f },
        {  0.0f, 1.0f,  6.0f },
        {  5.0f, 1.0f, -3.0f },
        { -5.0f, 1.0f, -4.0f },
    };
    for (const XMFLOAT3& c : centers) {
        m_cubeCenters.push_back(c);
        m_obstacles.push_back(AABB{
            { c.x - 1.0f, c.y - 1.0f, c.z - 1.0f },
            { c.x + 1.0f, c.y + 1.0f, c.z + 1.0f }
        });
    }

    // --- 外部モデル (.obj) を 1 体読み込む（PROMPT-V3-02）---
    //  assets/models/test_model.obj を読み込み、プレイヤー前方の床上に置く。
    //  読み込めない場合はキューブメッシュで代替（フォールバック）する。
    {
        std::vector<Vertex>   mv;
        std::vector<uint32_t> mi;
        if (ObjLoader::Load("assets/models/test_model.obj", mv, mi)) {
            m_modelMesh = renderer.CreateMesh(mv, mi);
            m_hasModel  = true;
            LOG_INFO("外部モデルを配置しました（assets/models/test_model.obj）");
        } else {
            LOG_WARN("assets/models/test_model.obj を読み込めません。キューブで代替します。");
            m_hasModel = false;
        }
    }
}

void World::LoadGltfCharacter(Renderer& renderer) {
    // --- 外部モデル (.glb / glTF 2.0) を 1 体読み込む（ステージL-01a）---
    //  .obj と違い、1 ファイルの中にノード階層・複数マテリアルを持てる。
    //  ここでは primitive（マテリアルごとの塊）単位でメッシュを作る。
    //  読めなくてもゲームは成立するので、失敗しても警告だけで続行する。
    {
        // ステージL-04b: GltfModel は m_gltfModel（Worldのメンバ）に読み込む。
        // ★ローカル変数にしてはいけない★ AnimationPlayer はノード階層・スキン・
        //   アニメーションデータへの const ポインタを持ち続けるので、Build() を抜けた後も
        //   参照先が生きている必要がある。
        GltfModel& gltf = m_gltfModel;
        std::string err;
        // ステージL-04b: スキン付きの Swat（Quaternius 系・79 ボーン・24 クリップ）を最優先。
        // 無ければ L-01b のテクスチャ兵士、それも無ければ L-01a の検証用モデル。
        // CC0 / 個人所有のアセットを入手したら、この配列の先頭に置けばそのまま差し替わる
        //   （glTF なら読み込み側の変更は不要。詳細は assets/CREDITS.md）。
        const char* kCandidates[] = {
            "assets/models/external/Swat.glb",
            "assets/models/soldier.glb",
            "assets/models/test_figure.glb",
        };
        const char* kGltfPath = nullptr;
        for (const char* cand : kCandidates) {
            if (LoadGltf(cand, gltf, &err)) { kGltfPath = cand; break; }
        }
        if (kGltfPath != nullptr) {
            // ステージL-01b: デコード済み画像を GPU テクスチャにする。
            // 同じ画像を複数 primitive が参照することがあるので、画像インデックスで
            // キャッシュして GPU 上の重複生成を避ける。
            std::vector<Texture> gpuImages(gltf.images.size());
            std::vector<bool>    gpuImageMade(gltf.images.size(), false);

            // 画像インデックス → GPU テクスチャ（無ければ作る）。同じ画像を複数の
            // primitive／複数のスロットが参照するので、ここで一度だけ作る。
            auto gpuTexture = [&](int imgIndex) -> const Texture* {
                if (imgIndex < 0 || static_cast<size_t>(imgIndex) >= gltf.images.size()) {
                    return nullptr;
                }
                const size_t ii = static_cast<size_t>(imgIndex);
                if (!gltf.images[ii].valid) return nullptr;
                if (!gpuImageMade[ii]) {
                    const GltfImage& img = gltf.images[ii];
                    gpuImages[ii] = renderer.CreateTexture(img.width, img.height,
                                                           img.rgba.data());
                    gpuImageMade[ii] = true;
                }
                return gpuImages[ii].valid ? &gpuImages[ii] : nullptr;
            };

            const bool pbrOk    = renderer.IsPbrAvailable();
            const bool skinOk   = renderer.IsSkinningAvailable();
            m_gltfParts.reserve(gltf.primitives.size());
            int pbrCount = 0, texturedCount = 0, skinnedCount = 0;
            int firstSkinIndex = -1;

            for (const GltfPrimitive& prim : gltf.primitives) {
                GltfPart part;
                const Texture* baseTex = prim.hasUv ? gpuTexture(prim.baseColorImage) : nullptr;

                if (pbrOk) {
                    // ステージL-02: PBR 経路。テクスチャが無くても係数だけで描けるので、
                    // PBR が使える環境では常にこちらを通す（金属/粗さの表現が効く）。
                    const Texture* nrmTex = prim.hasUv ? gpuTexture(prim.normalImage) : nullptr;
                    const Texture* ormTex = prim.hasUv ? gpuTexture(prim.ormImage)    : nullptr;

                    part.pbrMaterial = renderer.CreatePbrMaterial(baseTex, nrmTex, ormTex);
                    if (part.pbrMaterial.valid) {
                        part.pbrMaterial.baseColorFactor  = prim.baseColor;
                        part.pbrMaterial.metallicFactor   = prim.metallicFactor;
                        part.pbrMaterial.roughnessFactor  = prim.roughnessFactor;
                        part.pbrMaterial.normalScale      = prim.normalScale;

                        // ステージL-04b: スキン付き primitive は GPU スキニング経路へ。
                        if (prim.skinned && skinOk) {
                            part.mesh = renderer.CreateSkinnedPbrMesh(
                                MakeSkinnedPbrVertices(prim), prim.indices);
                            part.usesSkinning = true;
                            if (firstSkinIndex < 0) firstSkinIndex = prim.skinIndex;
                            ++skinnedCount;
                        } else {
                            part.mesh = renderer.CreatePbrMesh(MakePbrVertices(prim), prim.indices);
                        }
                        part.usePbr = true;
                        ++pbrCount;
                    }
                }

                if (!part.usePbr) {
                    // フォールバック: テクスチャがあれば従来のテクスチャ経路、無ければ単色。
                    // ※スキン付きモデルが PBR 不可環境に来た場合、ここではバインドポーズの
                    //   まま静止表示される（スキニングは Pbr 系にしか実装していないため）。
                    if (baseTex != nullptr) {
                        part.texture  = *baseTex;
                        part.textured = true;
                        part.mesh = renderer.CreateTexturedMesh(prim.vertices, prim.indices);
                        ++texturedCount;
                    } else {
                        part.mesh = renderer.CreateMesh(MakeLitVertices(prim), prim.indices);
                    }
                }
                m_gltfParts.push_back(std::move(part));
            }
            m_hasGltf = !m_gltfParts.empty();

            // ステージL-04b: スキン付き primitive が 1 つでもあれば、各 Enemy/Hider が
            // 個別の AnimationPlayer でアニメーションを再生できる。
            m_gltfHasSkin  = (skinnedCount > 0) && (firstSkinIndex >= 0);
            m_gltfSkinIndex = firstSkinIndex; // 敵/hider が個別の AnimationPlayer を Bind する際に使う
            if (m_gltfHasSkin) {
                LOG_INFO("glTF のスキン／アニメーションを読み込みました（%zu クリップ）。"
                         "敵・hider ごとに個別再生します。",
                         m_gltfModel.animations.size());
            }

            LOG_INFO("glTF モデルを配置しました（%s: primitive %zu（PBR %d / スキン %d / "
                     "テクスチャ %d）/ 頂点 %zu / 画像 %zu / 高さ %.2fm）",
                     kGltfPath, gltf.primitives.size(), pbrCount, skinnedCount, texturedCount,
                     gltf.totalVertices, gltf.images.size(),
                     gltf.boundsMax.y - gltf.boundsMin.y);
        } else {
            LOG_WARN("glTF モデルを読み込めません（最後のエラー: %s）。"
                     "python tools/make_soldier_glb.py assets/models/soldier.glb を実行してください。",
                     err.c_str());
            m_hasGltf = false;
        }
    }
}

void World::Update(float dt) {
    // アニメーション時間は敵／hider 個体ごとに各モードで更新する。
    (void)dt;
}

void World::Render(Renderer& renderer) {
    const bool arena =
        (m_layout == WorldLayout::FpsArena || m_layout == WorldLayout::HideArena);

    // 背景の空を最初に描く（深度書き込みなし＝後続の床/物体が上書きする）。
    // 空はフォグ対象外（フォグを一時 OFF）。フォグさせると縦グラデーションが潰れるため。
    if (arena) {
        renderer.SetFog(false);
        renderer.DrawMesh(m_skyMesh, XMMatrixIdentity(), /*ignoreDepth=*/true);
        renderer.SetFog(true);
    }

    // 床（arena はテクスチャ地面、Default は頂点色）。
    if (arena && m_floorTextured) {
        renderer.DrawTexturedMesh(m_floorTexMesh, m_floorTex, XMMatrixIdentity());
    } else {
        renderer.DrawMesh(m_floorMesh, XMMatrixIdentity());
    }

    if (arena) {
        // 建築モジュール（Modular Parts）で組んだアリーナ。読み込めた場合はこちらが本命で、
        // m_boxes（手続き生成の箱）は空になっているので下のループは何もしない。
        for (const ModuleInstance& mi : m_moduleInstances) {
            m_modules[mi.kind].Draw(renderer, XMLoadFloat4x4(&mi.world));
        }

        // 壁／木箱／柱を描く。テクスチャ有効なら実寸メッシュ＋レンガ/木箱テクスチャ、
        // 無効なら単色の単位立方体を half 倍スケールしてフォールバック。
        for (const Box& b : m_boxes) {
            if (m_boxesTextured) {
                const Texture& tex = (b.mesh == 0) ? m_wallTex : m_crateTex;
                renderer.DrawTexturedMesh(
                    b.texMesh, tex,
                    XMMatrixTranslation(b.center.x, b.center.y, b.center.z));
            } else {
                const Mesh& mesh = (b.mesh == 0) ? m_wallMesh : m_crateMesh;
                const XMMATRIX world =
                    XMMatrixScaling(b.half.x, b.half.y, b.half.z) *
                    XMMatrixTranslation(b.center.x, b.center.y, b.center.z);
                renderer.DrawMesh(mesh, world);
            }
        }

        return;
    }

    // Default レイアウト（従来通り）。
    for (const XMFLOAT3& c : m_cubeCenters) {
        renderer.DrawMesh(m_cubeMesh, XMMatrixTranslation(c.x, c.y, c.z));
    }
    // 外部モデル（無ければキューブで代替）をプレイヤー前方に描画
    const XMFLOAT3& m = m_modelCenter;
    const Mesh& modelMesh = m_hasModel ? m_modelMesh : m_cubeMesh;
    renderer.DrawMesh(modelMesh, XMMatrixTranslation(m.x, m.y, m.z));

}

// Swat 化した敵/hider の向き補正。エンジンの yaw=0 は +Z 方向だが、モデルの「正面」が
// エクスポート元の都合で必ずしも +Z を向いているとは限らない。歩行方向と見た目の向きが
// ずれて見えたら、まずここを調整する（例: 後ろ向きに歩いて見えるなら XM_PI を足す）。
constexpr float kGltfCharacterYawOffset = 0.0f;

// 敵/hider を指定位置・向きで描く（DrawGltfModel の「敵ドロー版」）。
// DrawGltfModel との違いは3つ: ①yaw 回転を持つ（敵は歩く向きに正対する）②アニメーション
// 状態を呼び出し側が個別に持つ anim から取る
// （敵ごとに独立した巡回/追跡アニメを再生するため）③DrawEnemyTagged 系（敵専用の継ぎ目）
// を経由するので、ウォールハック実演・防御・段[5]検知が効く。
void World::DrawGltfCharacterTagged(Renderer& renderer, const DirectX::XMFLOAT3& position,
                                    float yaw, const AnimationPlayer& anim, float scale) {
    if (!m_hasGltf) return;
    // スケールは回転より先（モデル空間で縮めてから向きを付けて置く）。一様スケールなので
    // 法線変換 (float3x3)world も向きは保たれる（長さはシェーダー側で正規化される）。
    const XMMATRIX world = XMMatrixScaling(scale, scale, scale) *
                          XMMatrixRotationY(yaw + kGltfCharacterYawOffset) *
                          XMMatrixTranslation(position.x, position.y, position.z);
    // ★パーツごとではなくキャラクター（＝この Enemy/Hider 個体）につき 1 回だけ
    //   アップロードする★ 複数体を描くこのパスでパーツごとに呼ぶと、1 フレームの上限
    //   （kMaxSkinnedDrawsPerFrame=32）を敵 3〜4 体だけで使い切ってしまう。
    const D3D12_GPU_VIRTUAL_ADDRESS skinAddr = m_gltfHasSkin
        ? renderer.UploadSkinPaletteOnce(anim.JointMatrices()) : 0;
    for (const GltfPart& part : m_gltfParts) {
        if (part.usesSkinning) {
            renderer.DrawEnemyTaggedSkinned(part.mesh, part.pbrMaterial, skinAddr, world);
        } else if (part.usePbr) {
            // 非スキンの PBR パーツ（現行の Swat.glb には存在しないが将来のモデル用の保険）。
            // 敵ドロー専用の継ぎ目には乗らない＝この部分だけはウォールハックの対象外になる。
            renderer.DrawPbrMesh(part.mesh, part.pbrMaterial, world);
        } else if (part.textured) {
            renderer.DrawTexturedMesh(part.mesh, part.texture, world);
        } else {
            renderer.DrawMesh(part.mesh, world);
        }
    }
}

void World::RenderHidden(Renderer& renderer, bool wallhack) {
    // Default レイアウトの旧ウォールハック実演専用。FpsArena では隠し箱を持たない。
    if (m_layout != WorldLayout::Default) return;
    // 床・遮蔽物を描いた後に最後に描く。
    //   wallhack=false: 通常の深度テスト → 赤い箱に隠れて見えない
    //   wallhack=true : 深度無視で描画 → 赤い箱を上書きして透けて見える
    const XMFLOAT3& c = m_hiddenCenter;
    renderer.DrawMesh(m_hiddenMesh, XMMatrixTranslation(c.x, c.y, c.z),
                      /*ignoreDepth=*/wallhack);
}

void World::LoadArenaModules(Renderer& renderer, const ArenaModule* kinds, int count) {
    m_modulesLoaded = false;
    int ok = 0;
    for (int i = 0; i < count; ++i) {
        const int idx = static_cast<int>(kinds[i]);
        if (m_modules[idx].Load(renderer, kModuleSpecs[idx].path)) ++ok;
    }
    // ★全部そろったときだけ採用する★ 一部だけ読めた状態で組むと、外周に穴が空いた
    //   マップや当たり判定だけあって見えない壁ができてしまう。中途半端な状態を作るより、
    //   従来の手続き生成アリーナへまるごと戻したほうが挙動が読める。
    m_modulesLoaded = (ok == count);
    if (m_modulesLoaded) {
        LOG_INFO("配置モジュールを読み込みました（%d 種）", ok);
    } else {
        LOG_WARN("配置モジュールが %d/%d 種しか読めませんでした。"
                 "従来の手続き生成アリーナで構築します。", ok, count);
    }
}

void World::AddModule(ArenaModule kind, float x, float z, float yawDeg, bool solid) {
    const int idx = static_cast<int>(kind);
    const ModuleSpec& s = kModuleSpecs[idx];
    const GltfProp&   p = m_modules[idx];
    if (!p.IsLoaded()) return;

    const XMFLOAT3& bmin = p.BoundsMin();
    const XMFLOAT3& bmax = p.BoundsMax();
    const float yaw = XMConvertToRadians(yawDeg);

    // ①原点補正（自動）: 水平は中心へ、垂直は「底面が y=0」へ。
    //   モジュールごとに原点がばらばらなので、バウンディングから毎回計算する。
    const XMMATRIX pivot = XMMatrixTranslation(-(bmin.x + bmax.x) * 0.5f,
                                               -bmin.y,
                                               -(bmin.z + bmax.z) * 0.5f);
    // ②中心まわりに yaw 回転 → ③設置位置へ平行移動（行ベクトル規約なので左から順に適用）。
    const XMMATRIX world = pivot * XMMatrixRotationY(yaw) * XMMatrixTranslation(x, 0.0f, z);

    ModuleInstance inst;
    inst.kind = idx;
    XMStoreFloat4x4(&inst.world, world);
    m_moduleInstances.push_back(inst);

    if (!solid || !s.solidDefault) return;

    // 原点補正後の実寸（接地済みなので高さは 0〜height）。
    const float width  = (bmax.x - bmin.x) * s.colScaleXZ;
    const float depth  = (bmax.z - bmin.z) * s.colScaleXZ;
    const float height = bmax.y - bmin.y;

    // 回転した箱を包む AABB（保守的）。90 度単位なら幅と奥行が入れ替わるだけだが、
    // 任意角にも対応できるよう |cos|,|sin| で合成しておく。
    const float c = fabsf(cosf(yaw)), sn = fabsf(sinf(yaw));
    const float hw = width * 0.5f, hd = depth * 0.5f;

    if (kind == ArenaModule::Door3) {
        // ★扉は開口部を空ける★ 一枚岩の AABB にすると扉を通り抜けられない。
        //   幅方向の中央 kDoorOpeningHalf*2 を空け、両脇の袖壁だけを衝突対象にする。
        const float sideHalf = (hw - kDoorOpeningHalf) * 0.5f;   // 片側の袖壁の半幅
        if (sideHalf > 0.01f) {
            const float off = kDoorOpeningHalf + sideHalf;  // 中心から袖壁の中心までの距離
            // 幅方向の単位ベクトル（yaw 回転後）。
            const float ux = cosf(yaw), uz = -sinf(yaw);
            for (int sign = -1; sign <= 1; sign += 2) {
                const float cx = x + ux * off * static_cast<float>(sign);
                const float cz = z + uz * off * static_cast<float>(sign);
                const float sx = c * sideHalf + sn * hd;
                const float sz = sn * sideHalf + c * hd;
                m_obstacles.push_back(AABB{ { cx - sx, 0.0f, cz - sz },
                                            { cx + sx, height, cz + sz } });
            }
        }
        return;
    }

    const float ex = c * hw + sn * hd;   // X 方向の半径
    const float ez = sn * hw + c * hd;   // Z 方向の半径
    m_obstacles.push_back(AABB{ { x - ex, 0.0f, z - ez },
                                { x + ex, height, z + ez } });
}

void World::AddPerimeter(ArenaModule kind, float halfExtent, float moduleWidth) {
    // 各辺を等間隔に敷き詰める。割り切れないモジュール幅でも隙間が偏らないよう、
    // 「個数を四捨五入で決めてから間隔を辺長／個数で求め直す」方式にする
    // （わずかに重なる or わずかに空くが、対称になるので目立たない）。
    const int n = (std::max)(1, static_cast<int>(halfExtent * 2.0f / moduleWidth + 0.5f));
    const float step = halfExtent * 2.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const float c = -halfExtent + step * (static_cast<float>(i) + 0.5f);
        AddModule(kind,  c,  halfExtent, 180.0f); // 北（面を -Z へ）
        AddModule(kind,  c, -halfExtent,   0.0f); // 南（面を +Z へ）
        AddModule(kind,  halfExtent,  c, 270.0f); // 東（面を -X へ）
        AddModule(kind, -halfExtent,  c,  90.0f); // 西（面を +X へ）
    }
}

void World::BuildFpsArenaModules() {
    m_moduleInstances.clear();
    m_obstacles.clear();

    const float R = m_floorSize * 0.5f;          // 外周（床は -R..R）。既定 20m

    // --- 外周壁: 各辺 40m を 5m 壁で埋める ---
    // AddPerimeter の yaw は「テクスチャの表側を内側へ向ける」意図で決めている。逆を
    // 向いていたら AddPerimeter 内の各辺の値に 180 を足す（Ext モジュールは片面だけ
    // 作り込まれているため）。
    AddPerimeter(ArenaModule::Wall5, R, 5.0f);

    // --- 中央の建物（13m x 10m）---
    // 南に扉（侵入経路）、北に窓（射線）を置いて「入れる/覗ける」構造にする。
    // 南北の面は 5m 壁 + 3m 開口 + 5m 壁 = 13m。東西の面は 5m 壁 2 枚 = 10m。
    constexpr float kBx = 0.0f, kBz = 0.0f;      // 建物の中心
    constexpr float kHalfW = 6.5f, kHalfD = 5.0f;
    AddModule(ArenaModule::Wall5,   kBx - 4.0f, kBz - kHalfD,   0.0f);
    AddModule(ArenaModule::Door3,   kBx,        kBz - kHalfD,   0.0f);
    AddModule(ArenaModule::Wall5,   kBx + 4.0f, kBz - kHalfD,   0.0f);
    AddModule(ArenaModule::Wall5,   kBx - 4.0f, kBz + kHalfD, 180.0f);
    AddModule(ArenaModule::Window3, kBx,        kBz + kHalfD, 180.0f);
    AddModule(ArenaModule::Wall5,   kBx + 4.0f, kBz + kHalfD, 180.0f);
    AddModule(ArenaModule::Wall5,   kBx + kHalfW, kBz - 2.5f, 270.0f);
    AddModule(ArenaModule::Wall5,   kBx + kHalfW, kBz + 2.5f, 270.0f);
    AddModule(ArenaModule::Wall5,   kBx - kHalfW, kBz - 2.5f,  90.0f);
    AddModule(ArenaModule::Wall5,   kBx - kHalfW, kBz + 2.5f,  90.0f);

    // --- 独立した遮蔽壁（L 字）: 開けた場所に射線を切る角を作る ---
    // ★配置は FpsMode の敵巡回ルート(kEnemySpawns)と両立させること★ 巡回の直線上に壁を
    //   置くと、敵が壁に押し付けられて往復できなくなる（AI に経路探索が無く、目標へ直進
    //   するだけのため）。巡回は建物の周り（x=±9 / z=±8）と北の縦道(x=0)に通してあるので、
    //   遮蔽物はそれより外側（|x|>11 または |z|>11）にだけ置く。
    struct Segment { float x, z, yaw; };
    const Segment kCoverWalls[] = {
        { -14.0f,  12.0f,   0.0f }, { -16.5f,   9.5f,  90.0f },  // 北西の L
        {  14.0f, -12.0f, 180.0f }, {  16.5f,  -9.5f, 270.0f },  // 南東の L
        { -14.0f, -13.0f,   0.0f },                              // 南西の衝立
        {  14.0f,  13.0f, 180.0f },                              // 北東の衝立
    };
    for (const Segment& seg : kCoverWalls) AddModule(ArenaModule::Wall5, seg.x, seg.z, seg.yaw);

    // --- カバー用の置物（コンテナ＝腰高の遮蔽、バレル＝小さい遮蔽）---
    // こちらも巡回レーンを避けて配置する（上記と同じ理由）。
    struct Prop { ArenaModule kind; float x, z, yaw; };
    const Prop kProps[] = {
        { ArenaModule::Container, -13.0f,   3.0f,  15.0f },
        { ArenaModule::Container,  13.0f,  -3.0f, 200.0f },
        { ArenaModule::Container,  17.0f,   8.0f,  90.0f },
        { ArenaModule::Container, -17.0f,  -8.0f, 270.0f },
        { ArenaModule::Container,   4.0f,  16.0f,  45.0f },
        { ArenaModule::Barrel,    -11.5f,  -3.5f,   0.0f },
        { ArenaModule::Barrel,    -12.2f,  -4.2f,  30.0f },
        { ArenaModule::Barrel,     12.0f,   5.0f,   0.0f },
        { ArenaModule::Barrel,     17.0f, -14.0f,  60.0f },
        { ArenaModule::Barrel,    -17.0f,  14.0f,  10.0f },
        { ArenaModule::Barrel,      5.0f, -16.0f,   0.0f },
        { ArenaModule::Barrel,      5.8f, -16.6f,  40.0f },
    };
    for (const Prop& p : kProps) AddModule(p.kind, p.x, p.z, p.yaw);

    LOG_INFO("モジュール式アリーナを構築しました（モジュール %zu 個 / 当たり判定 %zu 個）",
             m_moduleInstances.size(), m_obstacles.size());
}

void World::BuildFpsArena(Renderer& renderer) {
    // 建築モジュールが全種そろっていれば、そちらでアリーナを組む（見た目の主役）。
    const ArenaModule kNeeded[] = {
        ArenaModule::Wall5, ArenaModule::Door3, ArenaModule::Window3,
        ArenaModule::Barrel, ArenaModule::Container,
    };
    LoadArenaModules(renderer, kNeeded,
                     static_cast<int>(sizeof(kNeeded) / sizeof(kNeeded[0])));
    if (m_modulesLoaded) {
        BuildFpsArenaModules();
        return;
    }
    // 以降はフォールバック（従来の手続き生成の箱アリーナ）。
    // フォールバック用の単色メッシュ（テクスチャ生成に失敗した時のみ使う）。
    {
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, XMFLOAT4{ 0.55f, 0.55f, 0.60f, 1.0f }, v, idx);
        m_wallMesh = renderer.CreateMesh(v, idx);
    }
    {
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, XMFLOAT4{ 0.55f, 0.38f, 0.20f, 1.0f }, v, idx);
        m_crateMesh = renderer.CreateMesh(v, idx);
    }
    // 視覚増分2: レンガ壁／木箱テクスチャを生成（WRAP サンプラーでタイリング）。
    {
        std::vector<uint8_t> px;
        groundtex::RenderBrick(256, 256, px);
        m_wallTex = renderer.CreateTexture(256, 256, px.data());
        groundtex::RenderWoodCrate(256, 256, px);
        m_crateTex = renderer.CreateTexture(256, 256, px.data());
        m_boxesTextured = m_wallTex.valid && m_crateTex.valid;
    }

    // 40x40 闘技場（床は -20..20）。外周壁でプレイ領域を囲み、
    // 内部に壁セグメントと木箱を置いて射線を切る／カバーを作る。
    m_boxes.clear();
    m_obstacles.clear();
    auto add = [&](float cx, float cy, float cz, float hx, float hy, float hz, int mesh) {
        Box b;
        b.center = { cx, cy, cz };
        b.half   = { hx, hy, hz };
        b.mesh   = mesh;
        // 実寸で UV タイリングしたテクスチャメッシュを用意（tpm=0.5＝1タイル/2m）。
        if (m_boxesTextured) b.texMesh = MakeTexBox(renderer, hx, hy, hz, 0.5f);
        m_boxes.push_back(std::move(b));
        m_obstacles.push_back(AABB{ { cx - hx, cy - hy, cz - hz },
                                    { cx + hx, cy + hy, cz + hz } });
    };

    // 外周壁（高さ 3m = half 1.5, 中心 y=1.5）。灰(0)。
    add(  0.0f, 1.5f,  19.5f, 20.0f, 1.5f, 0.5f, 0); // 北
    add(  0.0f, 1.5f, -19.5f, 20.0f, 1.5f, 0.5f, 0); // 南
    add( 19.5f, 1.5f,   0.0f, 0.5f, 1.5f, 20.0f, 0); // 東
    add(-19.5f, 1.5f,   0.0f, 0.5f, 1.5f, 20.0f, 0); // 西

    // 内部の壁セグメント（射線を切る）。灰(0)。
    add( -6.0f, 1.5f,   4.0f, 0.6f, 1.5f,  6.0f, 0);
    add(  6.0f, 1.5f,  -4.0f, 0.6f, 1.5f,  6.0f, 0);
    add(  4.0f, 1.5f,  11.0f, 6.0f, 1.5f,  0.6f, 0);
    add( -4.0f, 1.5f, -11.0f, 6.0f, 1.5f,  0.6f, 0);

    // カバー用の木箱（2x2x2 = half 1）。茶(1)。
    add(  0.0f, 1.0f,   0.0f, 1.0f, 1.0f, 1.0f, 1);
    add( 11.0f, 1.0f,   8.0f, 1.0f, 1.0f, 1.0f, 1);
    add(-11.0f, 1.0f,  -8.0f, 1.0f, 1.0f, 1.0f, 1);
    add( 13.0f, 1.0f, -12.0f, 1.0f, 1.0f, 1.0f, 1);
    add(-13.0f, 1.0f,  11.0f, 1.0f, 1.0f, 1.0f, 1);
    add(  8.0f, 1.0f,  -2.0f, 1.0f, 1.0f, 1.0f, 1);
    add( -8.0f, 1.0f,   2.0f, 1.0f, 1.0f, 1.0f, 1);
    add( -2.0f, 1.0f,  14.0f, 1.0f, 1.0f, 1.0f, 1);
}

void World::BuildHideArenaModules() {
    m_moduleInstances.clear();
    m_obstacles.clear();

    const float R = m_floorSize * 0.5f;   // 既定 20m（床は -R..R）

    // --- 外周: レンガのフェンスで園庭をぐるりと囲う ---
    AddPerimeter(ArenaModule::Fence3, R, 3.3f);

    // --- 遊具エリア（中央〜北寄り）---
    // かくれんぼは「近づかないと見つからない」ゲームなので、視線を切る構造物を
    // 中央に散らして、探索の動線が一本道にならないようにする。
    AddModule(ArenaModule::Slide,        -3.0f,   2.0f, 200.0f);
    AddModule(ArenaModule::Slide,        10.0f,  -6.0f,  20.0f);
    AddModule(ArenaModule::Swing,         4.0f,   9.0f,   0.0f);  // 下をくぐれる（当たり判定なし）
    AddModule(ArenaModule::Swing,       -11.0f,  -9.0f,  90.0f);
    AddModule(ArenaModule::SpringHorse,   1.0f,   3.5f,  40.0f);
    AddModule(ArenaModule::SpringHorse,   2.2f,   4.6f, 210.0f);
    AddModule(ArenaModule::SpringHorse,  -8.0f,  12.0f, 120.0f);

    // --- ベンチとテーブル（腰高の遮蔽＝しゃがんだ子供が隠れられる）---
    struct Prop { ArenaModule kind; float x, z, yaw; };
    const Prop kFurniture[] = {
        { ArenaModule::Bench,  -7.0f,  -2.0f,   0.0f },
        { ArenaModule::Bench,   7.0f,   4.0f, 180.0f },
        { ArenaModule::Bench,  14.0f, -13.0f,  90.0f },
        { ArenaModule::Bench, -14.0f,   7.0f, 270.0f },
        { ArenaModule::Table,   0.0f, -10.0f,   0.0f },
        { ArenaModule::Table,  13.0f,  10.0f,  30.0f },
        { ArenaModule::Table, -13.0f, -14.0f, 200.0f },
    };
    for (const Prop& f : kFurniture) AddModule(f.kind, f.x, f.z, f.yaw);

    // --- 植栽（かくれんぼの主役。茂みの陰が主な隠れ場所になる）---
    const Prop kPlants[] = {
        { ArenaModule::BushBig,    -16.0f,  -4.0f,   0.0f },
        { ArenaModule::BushBig,     16.0f,   2.0f,  90.0f },
        { ArenaModule::BushBig,     -5.0f, -15.0f, 200.0f },
        { ArenaModule::BushBig,      8.0f,  15.0f,  40.0f },
        { ArenaModule::BushBig,    -12.0f,  16.0f, 120.0f },
        { ArenaModule::BushSmall,  -16.5f,  -7.0f,   0.0f },
        { ArenaModule::BushSmall,   16.5f,   5.5f,  60.0f },
        { ArenaModule::BushSmall,   -2.0f, -16.5f,  30.0f },
        { ArenaModule::BushSmall,   11.0f,  16.5f, 150.0f },
        { ArenaModule::BushSmall,  -15.0f,  13.0f, 210.0f },
        { ArenaModule::BushSmall,    5.5f,  -4.0f, 300.0f },
        { ArenaModule::Tree,       -10.0f,   6.0f,   0.0f },
        { ArenaModule::Tree,        13.0f,  -2.0f, 120.0f },
        { ArenaModule::Tree,        -3.0f,  17.0f, 240.0f },
        { ArenaModule::Tree,        17.0f, -16.0f,  60.0f },
        { ArenaModule::Tree,       -17.0f, -16.0f, 300.0f },
    };
    for (const Prop& p : kPlants) AddModule(p.kind, p.x, p.z, p.yaw);

    LOG_INFO("公園（かくれんぼ）をモジュールで構築しました（モジュール %zu 個 / 当たり判定 %zu 個）",
             m_moduleInstances.size(), m_obstacles.size());
}

void World::BuildHideArena(Renderer& renderer) {
    // 公園モジュールが全種そろっていれば、そちらで園庭を組む（見た目の主役）。
    const ArenaModule kNeeded[] = {
        ArenaModule::Fence3, ArenaModule::Slide, ArenaModule::Swing,
        ArenaModule::SpringHorse, ArenaModule::Bench, ArenaModule::Table,
        ArenaModule::BushBig, ArenaModule::BushSmall, ArenaModule::Tree,
    };
    LoadArenaModules(renderer, kNeeded,
                     static_cast<int>(sizeof(kNeeded) / sizeof(kNeeded[0])));
    if (m_modulesLoaded) {
        BuildHideArenaModules();
        return;
    }
    // 以降はフォールバック（従来の手続き生成の箱アリーナ）。
    // かくれんぼ用の広い場（40x40）。FPS より密で多様な隠れ場所（大小の木箱・壁・柱）を置く。
    // フォールバック用の単色メッシュ。
    {
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, XMFLOAT4{ 0.55f, 0.55f, 0.60f, 1.0f }, v, idx);
        m_wallMesh = renderer.CreateMesh(v, idx);
    }
    {
        std::vector<Vertex> v; std::vector<uint32_t> idx;
        MakeBox(1.0f, 1.0f, 1.0f, XMFLOAT4{ 0.55f, 0.38f, 0.20f, 1.0f }, v, idx);
        m_crateMesh = renderer.CreateMesh(v, idx);
    }
    // 視覚増分2: レンガ壁／木箱テクスチャ（FPS と同じ）。
    {
        std::vector<uint8_t> px;
        groundtex::RenderBrick(256, 256, px);
        m_wallTex = renderer.CreateTexture(256, 256, px.data());
        groundtex::RenderWoodCrate(256, 256, px);
        m_crateTex = renderer.CreateTexture(256, 256, px.data());
        m_boxesTextured = m_wallTex.valid && m_crateTex.valid;
    }

    m_boxes.clear();
    m_obstacles.clear();
    auto add = [&](float cx, float cy, float cz, float hx, float hy, float hz, int mesh) {
        Box b;
        b.center = { cx, cy, cz };
        b.half   = { hx, hy, hz };
        b.mesh   = mesh;
        if (m_boxesTextured) b.texMesh = MakeTexBox(renderer, hx, hy, hz, 0.5f);
        m_boxes.push_back(std::move(b));
        m_obstacles.push_back(AABB{ { cx - hx, cy - hy, cz - hz },
                                    { cx + hx, cy + hy, cz + hz } });
    };

    // 外周壁（高さ3m）。灰(0)。
    add(  0.0f, 1.5f,  19.5f, 20.0f, 1.5f, 0.5f, 0);
    add(  0.0f, 1.5f, -19.5f, 20.0f, 1.5f, 0.5f, 0);
    add( 19.5f, 1.5f,   0.0f, 0.5f, 1.5f, 20.0f, 0);
    add(-19.5f, 1.5f,   0.0f, 0.5f, 1.5f, 20.0f, 0);

    // 長い低壁（腰高 2m）＝隠れやすい仕切り。灰(0)。
    add(  8.0f, 1.0f,   6.0f, 5.0f, 1.0f, 0.5f, 0);
    add( -8.0f, 1.0f,  -6.0f, 0.5f, 1.0f, 5.0f, 0);
    add( -3.0f, 1.0f,  10.0f, 0.5f, 1.0f, 4.0f, 0);
    add(  4.0f, 1.0f, -11.0f, 4.0f, 1.0f, 0.5f, 0);

    // 背の高い柱（高さ5m・細い）＝視線の切れ目。灰(0)。
    add( 12.0f, 2.5f,  13.0f, 0.5f, 2.5f, 0.5f, 0);
    add(-13.0f, 2.5f,  12.0f, 0.5f, 2.5f, 0.5f, 0);
    add( 14.0f, 2.5f,  -3.0f, 0.5f, 2.5f, 0.5f, 0);
    add(-11.0f, 2.5f, -13.0f, 0.5f, 2.5f, 0.5f, 0);

    // 大小の木箱（隠れ場所）。茶(1)。大(half1.6)・中(half1)・小(half0.7)を混在。
    add(  0.0f, 1.6f,   0.0f, 1.6f, 1.6f, 1.6f, 1); // 中央の大箱
    add( 10.0f, 1.0f,  -8.0f, 1.0f, 1.0f, 1.0f, 1);
    add(-10.0f, 1.0f,   6.0f, 1.0f, 1.0f, 1.0f, 1);
    add(  6.0f, 0.7f,  14.0f, 0.7f, 0.7f, 0.7f, 1);
    add( -6.0f, 0.7f, -14.0f, 0.7f, 0.7f, 0.7f, 1);
    add( 15.0f, 1.0f,   9.0f, 1.0f, 1.0f, 1.0f, 1);
    add(-15.0f, 0.7f,  -8.0f, 0.7f, 0.7f, 0.7f, 1);
    add(  2.0f, 0.7f, -16.0f, 0.7f, 0.7f, 0.7f, 1);
}
