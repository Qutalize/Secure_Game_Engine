#pragma once

#include <vector>
#include <DirectXMath.h>

#include "engine/math/Collision.h"
#include "engine/renderer/Animation.h"     // ステージL-04b
#include "engine/renderer/GltfLoader.h"    // ステージL-04b: GltfModel を保持し続けるため
#include "engine/renderer/Renderer.h"
#include "game/GltfProp.h"                 // アリーナ建築モジュール（Modular Parts）

// ステージを組み立てる配置モジュールの種別（Poly Universal Pack を glTF 化したもの）。
// 実寸と原点は .glb 自身のバウンディングから自動で読み取るので、ここに寸法は書かない
// （World.cpp の kModuleSpecs はパスと当たり判定の調整値だけを持つ）。
enum class ArenaModule {
    // --- FPS アリーナ（工業/軍事テーマ）---
    Wall5, Door3, Window3, Barrel, Container,
    // --- かくれんぼ（公園/幼稚園テーマ）---
    Fence3, Slide, Swing, SpringHorse, Bench, Table, BushBig, BushSmall, Tree,
    Count
};

// ワールドのレイアウト。
//   Default   : 従来の 20x20＋遮蔽物5個（既存互換。変更しない）。
//   FpsArena  : PROMPT-G-02 の広い 40x40 闘技場（外周壁＋内部カバー）。
//   HideArena : PROMPT-G-03 の広い 40x40 かくれんぼ場（多様な隠れ場所：木箱/壁/柱）。
enum class WorldLayout { Default, FpsArena, HideArena };

// ゲームワールド。床と遮蔽物（立方体・壁）を持つ。
class World {
public:
    // layout 省略時は従来レイアウト（既存呼び出し＝かくれんぼは無改造で動く）。
    void Build(Renderer& renderer, WorldLayout layout = WorldLayout::Default);

    // 描画（床と遮蔽物）
    void Render(Renderer& renderer);

    // ステージL-04b: glTF モデルのアニメーションを進める。Build 後、毎フレーム呼ぶこと。
    void Update(float dt);

    // 隠し直方体を描画する。wallhack=true なら深度無視で遮蔽物越しに見える。
    // Default レイアウトのみ有効（FpsArena では何もしない）。
    void RenderHidden(Renderer& renderer, bool wallhack);

    // 衝突判定用の全 AABB リスト
    const std::vector<AABB>& GetObstacles() const { return m_obstacles; }

    float GetFloorSize() const { return m_floorSize; }

    static constexpr float kFloorSize = 20.0f; // Default レイアウトの床サイズ

    // --- 敵/hider の Swat 化（GltfModel/AnimationPlayer を外部と共有する）---
    // GltfModel 本体（メッシュ/スキン/アニメーションのデータ）は World が所有し続ける
    // （AnimationPlayer は const ポインタで参照するだけなので、World より先に破棄しないこと）。
    // 各 Enemy/Hider は「自分専用の AnimationPlayer インスタンス」を GltfCharacterModel() と
    // GltfCharacterSkinIndex() で Bind し、個体ごとに独立した再生位置/クリップを持つ。
    bool HasAnimatedCharacter() const { return m_hasGltf && m_gltfHasSkin; }
    const GltfModel& GltfCharacterModel() const { return m_gltfModel; }
    int GltfCharacterSkinIndex() const { return m_gltfSkinIndex; }

    // 敵/hider を指定位置・向きで描く。DrawEnemyTagged/DrawEnemyTaggedSkinned という
    // 「敵ドロー専用の継ぎ目」を通すので、ウォールハック実演・防御（門番・段[5]検知）が
    // Swat 化した敵にもそのまま効く（＝描画インターセプトの継ぎ目はキャラの見た目を問わない）。
    // 呼び出し側は HasAnimatedCharacter() が false のときは呼ばず、従来の箱モデルへ
    // フォールバックすること（PBR/スキニング非対応環境・glTF 読み込み失敗時の安全策）。
    //   scale : 一様スケール。かくれんぼの「子供役」を小柄にするのに使う（1.0 で等身大）。
    //           法線変換は (float3x3)world なので★一様スケールのみ対応★（非一様は歪む）。
    void DrawGltfCharacterTagged(Renderer& renderer, const DirectX::XMFLOAT3& position,
                                float yaw, const AnimationPlayer& anim, float scale = 1.0f);

private:
    void BuildFpsArena(Renderer& renderer);  // PROMPT-G-02: 広い闘技場を構築
    void BuildHideArena(Renderer& renderer); // PROMPT-G-03: 広いかくれんぼ場を構築

    // ステージL-01a/L-04b: 敵/hider が共有する外部 glTF(.glb) モデルの読み込み。
    // ★ Build() は FpsArena/HideArena だと BuildFpsArena/BuildHideArena の後で早期 return
    //   するため、この読み込みは必ずその return より前で呼ぶこと（呼び忘れるとどのレイアウトでも
    //   m_hasGltf が false のままになり、各モードは従来の箱モデルへフォールバックする）。
    void LoadGltfCharacter(Renderer& renderer);

    // --- 配置モジュール（Modular Parts / City / Nature）-----------------------
    // kinds に挙げた種類だけを読み込む。全部そろったときだけ m_modulesLoaded=true に
    // する（一部だけ欠けた状態で組むと穴の空いたマップになるので、全部か無しかにする）。
    void LoadArenaModules(Renderer& renderer, const ArenaModule* kinds, int count);

    // モジュールを 1 個配置する。world 行列（接地・中心合わせ＋yaw 回転＋平行移動）を
    // 作って m_moduleInstances へ積み、同時に衝突用 AABB を m_obstacles へ登録する。
    //   x, z    : 設置位置（モジュールの「見た目の水平中心」をここに合わせる）
    //   yawDeg  : Y 軸まわりの回転（度）。0 のときモジュールの幅は X 方向を向く
    //   solid   : false なら見た目だけ置いて当たり判定を作らない（走り抜けられる装飾）
    void AddModule(ArenaModule kind, float x, float z, float yawDeg, bool solid = true);

    // 外周をモジュールで一周囲む（各辺を等間隔に敷き詰める）。
    void AddPerimeter(ArenaModule kind, float halfExtent, float moduleWidth);

    // モジュールで FPS アリーナを組む（外周壁＋中央の建物＋カバー）。
    void BuildFpsArenaModules();

    // モジュールでかくれんぼの公園を組む（外周フェンス＋遊具＋植栽）。
    void BuildHideArenaModules();

    WorldLayout m_layout    = WorldLayout::Default;
    float       m_floorSize = kFloorSize;

    Mesh m_floorMesh;    // 頂点色の床（Default レイアウト用）
    Mesh m_floorTexMesh; // テクスチャ床（arena 用・タイリング UV）
    Texture m_floorTex;  // 地面テクスチャ（草/コンクリート）
    bool m_floorTextured = false; // arena はテクスチャ床を使う
    Mesh m_skyMesh;      // グラデーション空（PROMPT-G/StageI: 背景の空っぽ感を解消。arena のみ）
    Mesh m_cubeMesh;     // 単位立方体（half=1）。Default の遮蔽物・スケール描画に使う
    Mesh m_hiddenMesh;   // 隠し直方体（Default のみ）
    Mesh m_modelMesh;    // 外部 .obj モデル（PROMPT-V3-02, Default のみ）
    bool m_hasModel = false; // .obj 読み込み成否（false ならキューブで代替）

    // ステージL-01a/L-01b: 外部 glTF(.glb) モデル。glTF は 1 ファイルが複数 primitive
    // （＝マテリアルごとの塊）に分かれるので、描画単位も primitive ごとに持つ。
    //   テクスチャがある primitive → TexVertex メッシュ + Texture（DrawTexturedMesh）
    //   テクスチャが無い primitive → Vertex メッシュ（baseColor を頂点色に焼く / DrawMesh）
    // ステージL-02: PBR が使える環境では PBR 経路（法線マップ／金属度／粗さ）で描く。
    // 使えない場合は従来のテクスチャ／単色経路へフォールバックする。
    // ステージL-04b: スキン付き primitive はさらに GPU スキニング経路（usePbr かつ
    // usesSkinning）になる。
    struct GltfPart {
        Mesh        mesh;
        Texture     texture;
        PbrMaterial pbrMaterial;
        bool        textured    = false;
        bool        usePbr      = false;
        bool        usesSkinning = false;
    };
    std::vector<GltfPart> m_gltfParts;
    bool m_hasGltf = false;

    // ステージL-04b: モデル本体はここで保持し続ける（各 Enemy/Hider の AnimationPlayer が
    // 参照するため、Build() のローカル変数のままだとゲーム中に参照が消えて壊れる）。
    GltfModel       m_gltfModel;
    bool            m_gltfHasSkin  = false;
    int             m_gltfSkinIndex = -1; // 敵/hider の AnimationPlayer::Bind に渡す共有スキン番号

    std::vector<AABB>              m_obstacles;   // 衝突用
    std::vector<DirectX::XMFLOAT3> m_cubeCenters; // Default 描画用（立方体の中心）

    // FpsArena/HideArena 用: 壁とカバー木箱を大きさ可変で並べる。
    // 視覚増分2: レンガ/木箱テクスチャを貼る。各 Box は自分の実寸で UV タイリングした
    // テクスチャ付きメッシュ(texMesh)を持ち、DrawTexturedMesh で描く（テクスチャ不可なら
    // 単色メッシュ m_wallMesh/m_crateMesh を half 倍スケールしてフォールバック）。
    Mesh m_wallMesh;    // 灰色の単位立方体（フォールバック）
    Mesh m_crateMesh;   // 茶色の単位立方体（フォールバック）
    Texture m_wallTex;  // レンガ壁テクスチャ
    Texture m_crateTex; // 木箱テクスチャ
    bool    m_boxesTextured = false; // テクスチャ生成成功なら true
    struct Box {
        DirectX::XMFLOAT3 center;
        DirectX::XMFLOAT3 half;
        int  mesh;       // 0=wall(レンガ) / 1=crate(木箱)
        Mesh texMesh;    // この Box の実寸・UV タイリング済みテクスチャメッシュ
    };
    std::vector<Box> m_boxes;

    // --- アリーナ建築モジュール（Modular Parts）------------------------------
    // 種別ごとに .glb を 1 つ持ち、配置ぶんだけ world 行列を変えて描き回す。
    // ★読み込みに失敗したら m_modulesLoaded=false のままにして、従来の手続き生成の
    //   箱アリーナ（m_boxes）へまるごとフォールバックする★ アセットが無い環境でも
    //   ゲームとチート実演は成立させたいので、見た目の向上は「あれば使う」扱いにする。
    GltfProp m_modules[static_cast<int>(ArenaModule::Count)];
    bool     m_modulesLoaded = false;
    struct ModuleInstance {
        int                 kind;   // ArenaModule
        DirectX::XMFLOAT4X4 world;  // XMMATRIX はアライン要求があるので 4x4 で持つ
    };
    std::vector<ModuleInstance> m_moduleInstances;

    DirectX::XMFLOAT3 m_hiddenCenter{ 0.0f, 0.7f, 9.0f }; // 赤い箱(0,1,6)の後方
    DirectX::XMFLOAT3 m_modelCenter{ 0.0f, 0.0f, 2.0f };  // 外部モデルの設置位置（床上）
};
