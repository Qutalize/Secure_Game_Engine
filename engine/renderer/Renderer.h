#pragma once
#include "engine/core/EngineFeatures.h"
#if ENGINE_RAY_TRACING
#include "engine/renderer/RayTracing.h"
#include <memory>
#endif
#include "engine/security/SecurityMetrics.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>

#include "engine/renderer/Camera.h"

// 頂点フォーマット（位置 + 法線 + 色）。
// PROMPT-V3-02 でライティング対応のため normal を追加した（破壊的変更）。
// 入力レイアウト(CreatePipelineState)・全メッシュ生成・VS の VSInput と
// このレイアウトは一致させること。
struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 color;
};

// テクスチャ付き頂点（位置 + 法線 + UV）。トランプ絵柄など画像を貼る描画に使う。
// PROMPT-G-01 で追加。既存 Vertex（頂点色）とは別レイアウト＝混在させないこと。
// オフセットは position=0, normal=12, uv=24（入力レイアウトと一致させる）。
struct TexVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

// PBR 用頂点（位置 + 法線 + 接線 + UV）。ステージL-02 で追加。
// 接線(tangent)は法線マップを使うために必須。w は従法線の符号（UV ミラー面で -1 になる）。
// オフセット: position=0, normal=12, tangent=24, uv=40（入力レイアウトと一致させること）。
struct PbrVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 tangent;
    DirectX::XMFLOAT2 uv;
};

// スキン付き PBR 頂点（位置 + 法線 + 接線 + ボーン番号×4 + 重み×4 + UV）。ステージL-04b。
// joints は UINT4（0..127 のボーン番号）。DXGI_FORMAT_R32G32B32A32_UINT で渡す。
// オフセット: position=0, normal=12, tangent=24, joints=40, weights=56, uv=72（計 80B）。
struct SkinnedPbrVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 tangent;
    DirectX::XMUINT4  joints;
    DirectX::XMFLOAT4 weights;
    DirectX::XMFLOAT2 uv;
};

// 描画用メッシュ。頂点バッファとインデックスバッファを保持する。
// （アップロードヒープに直接置く簡易実装。学習用としては十分で、
//  本番では DEFAULT ヒープへコピーするのが望ましい旨をコメントで明示）
// ※ vbv.StrideInBytes は生成時に決まるため、Vertex 版でも TexVertex 版でも
//   この 1 つの構造体で表現できる（DrawMesh / DrawTexturedMesh で使い分ける）。
struct Mesh {
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW  ibv{};
    UINT indexCount = 0;
};

// GPU 上のテクスチャ（2D, RGBA8）と、その SRV のヒープ内インデックス。
// DrawTexturedMesh 時に srvIndex からデスクリプタテーブルを束ねる。
struct Texture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    UINT srvIndex = 0;   // m_srvHeap 内の位置
    bool valid    = false;
};

// PBR マテリアル（ステージL-02）。
// ★SRV デスクリプタテーブルは「連続した 3 枚」を要求する★ ので、
//   baseColor / normal / ORM を必ず連続したスロットに確保する（CreatePbrMaterial が担保）。
//   1 枚でも欠けるときは中立値のダミーテクスチャで埋める（白 / 平坦法線 / AO=1,rough,metal）。
struct PbrMaterial {
    Texture baseColor;
    Texture normal;
    Texture orm;                 // R=AO, G=roughness, B=metallic（glTF の標準的な詰め方）
    UINT    srvTableIndex = 0;   // 3 枚の先頭 SRV インデックス
    DirectX::XMFLOAT4 baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float metallicFactor  = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale     = 1.0f;
    bool  hasTextures     = false;
    bool  valid           = false;
};

// VS/PS と共有する定数バッファのレイアウト。256B アライン必須。
// 行列 3 つ(192B) + ライト情報 float4×4(64B) + ライト行列(64B) + シャドウ設定(16B) = 336B。
// alignas(256) なので sizeof は 512B に切り上げられる（＝定数バッファ 1 スロット = 512B）。
// HLSL 側 cbuffer SceneConstants（SimpleVS/PS・TexturedVS/PS・ShadowVS）とメンバ順を
// 一致させること。順序がずれると値が silent に化けるので、片方だけ足さないこと。
struct alignas(256) SceneConstants {
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT4X4 proj;
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4   lightDir;    // xyz: 平行光源が進む向き（正規化）, w: 未使用
    DirectX::XMFLOAT4   lightColor;  // rgb: 光の色, w: 強度
    DirectX::XMFLOAT4   ambient;     // rgb: 環境光, w: 未使用
    DirectX::XMFLOAT4   cameraPos;   // xyz: カメラ位置（スペキュラ計算用）, w: フォグ有効
    // ---- ステージL-03b（シャドウマップ）で追加 ----
    DirectX::XMFLOAT4X4 lightViewProj; // ライト視点のビュー×射影（PS が影を判定する）
    DirectX::XMFLOAT4   shadowParams;  // x=1/解像度, y=深度バイアス, z=有効フラグ, w=未使用
    // ---- ステージL-02（PBR）で追加 ----
    DirectX::XMFLOAT4   baseColorFactor;  // rgba
    DirectX::XMFLOAT4   materialParams;   // x=metallic, y=roughness, z=法線強度, w=テクスチャ有無
};

// 描画のタグ（インターセプト時に「これは敵ドローか」を判別する印。ステージJ-01）。
// 汎用 DrawMesh とは別に、敵は DrawEnemyTagged 経由で Enemy タグ付きで描く。
// このタグは本エンジン固有＝一般のゲームには無いので、これを頼るフックは非汎用になる。
enum class DrawTag  { Default, Enemy };

// 深度モード。ステージL-00 で Disabled を追加した。
//   Normal   … 深度テスト有効・LESS（正常な前後関係）
//   Always   … 深度テスト有効・ALWAYS（タイプA ウォールハック段[5]の再現）
//   Disabled … 深度テスト自体を無効（HUD/オーバーレイ・従来の ignoreDepth 経路）
// ※ Always と Disabled はどちらも「遮蔽を無視して描かれる」ため、敵ドローに対しては
//   門番（ResolveDepthMode）が両方まとめて拒絶する。新しい深度モードを足すときは
//   「Normal 以外は敵に許さない」という既定に自動的に乗るので、防御に穴が空かない。
enum class DepthMode { Normal, Always, Disabled };

// マテリアル種別（＝どのシェーダー／ルートシグネチャ／入力レイアウトで描くか）。
// ステージL-00 で導入。PSO は「マテリアル × 深度モード × スキニング × ブレンド」の
// 組み合わせで決まるので、その組み合わせをキーにしたキャッシュ（m_psoCache）で引く。
// L-02（PBR）や L-04（スキンドアニメ）で種別が増えても、ここに 1 行足すだけで
// 深度モードの全バリアントと門番が自動的に付いてくる。
// GPU へ渡せるジョイント行列の上限（ステージL-04b）。
// Swat.fbx は 79 本。一般的なヒューマノイドは 60〜120 本なので 128 で足りる。
// engine/renderer/Animation.h もこの値を使う（Renderer.h → Animation.h の順で
// include されるので、ここに置けば両方から見える）。
inline constexpr uint32_t kMaxJoints = 128;

enum class MaterialType {
    Lit,         // Vertex（位置/法線/頂点色）+ Blinn-Phong（SimpleVS/PS）
    Textured,    // TexVertex（位置/法線/UV）+ テクスチャ（TexturedVS/PS）
    Overlay,     // NDC 直描き・ライティングなし（OverlayVS/PS）
    Pbr,         // PbrVertex（位置/法線/接線/UV）+ baseColor/normal/ORM（PbrVS/PS）※L-02
    PbrSkinned,  // SkinnedPbrVertex + ジョイント行列パレット（PbrSkinnedVS/PbrPS）※L-04b
    Count
};

class Renderer {
public:
#if ENGINE_RAY_TRACING
#if ENGINE_RT_MESH
    uint32_t RegisterRayMesh(const geometry::TriangleMesh& mesh) { return m_rayScene->RegisterMesh(mesh); }
#if ENGINE_MODEL_ANIMATION
    void UpdateRayMesh(uint32_t id,const geometry::TriangleMesh& mesh) { m_rayScene->UpdateMesh(id,mesh); }
#endif
#endif
    void SetRayScene(const std::vector<RayTracingScene::Box>& boxes) { m_rayScene->Set(boxes); }
private:
    std::unique_ptr<RayTracingScene> m_rayScene;
public:
#endif
    Renderer() = default;
    ~Renderer();

    // basicOnly: 射撃場のLit+Overlayだけを初期化する。
    bool Initialize(HWND hwnd, int width, int height, bool basicOnly = false);

    // メッシュ生成（頂点 + 32bit インデックス）
    Mesh CreateMesh(const std::vector<Vertex>& vertices,
                    const std::vector<uint32_t>& indices);

    // 画面中央の照準（クロスヘア）メッシュを NDC で生成する（アスペクト比を補正）。
    // FPS / かくれんぼで「どこを向いているか」を示すために使う。
    Mesh CreateCrosshairMesh();

    // --- テクスチャ描画（PROMPT-G-01）---
    // RGBA8（1 ピクセル 4 バイト・行優先）の画像から GPU テクスチャを作る。
    // Init 中（フレーム外）に呼ぶ想定。内部で GPU コピー完了まで同期する。
    Texture CreateTexture(UINT width, UINT height, const uint8_t* rgba);

    // TexVertex（位置/法線/UV）+ 32bit インデックスからメッシュを作る。
    Mesh CreateTexturedMesh(const std::vector<TexVertex>& vertices,
                            const std::vector<uint32_t>& indices);

    // テクスチャを貼ってメッシュを描く。ignoreDepth=true で深度無視（透視）。
    void DrawTexturedMesh(const Mesh& mesh, const Texture& texture,
                          const DirectX::XMMATRIX& world, bool ignoreDepth = false);

    // --- PBR 描画（ステージL-02）---
    // PbrVertex（位置/法線/接線/UV）+ 32bit インデックスからメッシュを作る。
    Mesh CreatePbrMesh(const std::vector<PbrVertex>& vertices,
                       const std::vector<uint32_t>& indices);

    // PBR マテリアルを作る。nullptr の枠は中立値のダミーテクスチャで埋める
    //  （baseColor=白 / normal=平坦 / ORM=白＝AO1・粗さ1・金属1、係数で調整する）。
    // ★3 枚を SRV ヒープ上の連続した位置に確保する★（デスクリプタテーブルの要求）。
    //   テクスチャ本体は使い回し、SRV デスクリプタだけを 3 つ新規に書く。
    PbrMaterial CreatePbrMaterial(const Texture* baseColor,
                                  const Texture* normalMap,
                                  const Texture* orm);

    void DrawPbrMesh(const Mesh& mesh, const PbrMaterial& material,
                     const DirectX::XMMATRIX& world, bool ignoreDepth = false);

    // PBR が使えるか（シェーダーの取得に失敗した環境では false → 呼び出し側は従来経路へ）。
    bool IsPbrAvailable() const { return m_pbrRootSig != nullptr; }

    // --- スキンドアニメーション描画（ステージL-04b）---
    Mesh CreateSkinnedPbrMesh(const std::vector<SkinnedPbrVertex>& vertices,
                             const std::vector<uint32_t>& indices);

    // ジョイント行列パレットを一度だけアップロードし、その GPU アドレスを返す。
    // jointMatrices は「逆バインド行列 × 現在の世界変換」（AnimationPlayer::JointMatrices()
    // の生の出力・転置はこの関数の内部で行う）。要素数は kMaxJoints まで（超過分は無視）。
    // ★1 キャラクターが複数 primitive（パーツ）に分かれる場合は、パーツごとにではなく
    //   キャラクターにつき 1 回だけ呼ぶこと★ パーツごとに毎回アップロードすると、
    //   同じジョイント行列を primitive の数だけ複製するだけで、1 フレームの上限
    //   （kMaxSkinnedDrawsPerFrame）をキャラクター1体だけで使い切ってしまう（複数体の
    //   Swat 敵を描く「敵キャラの Swat 化」で実際にこれで枯渇した）。返ったアドレスを
    //   下記 DrawPbrSkinnedMesh / DrawEnemyTaggedSkinned の全パーツ呼び出しで使い回すこと。
    D3D12_GPU_VIRTUAL_ADDRESS UploadSkinPaletteOnce(
        const std::vector<DirectX::XMFLOAT4X4>& jointMatrices);

    // skinPaletteAddr は UploadSkinPaletteOnce が返したアドレス（同一キャラクターの
    // 全パーツで使い回す）。シャドウパスと本描画パスも同じアドレスを参照する。
    void DrawPbrSkinnedMesh(const Mesh& mesh, const PbrMaterial& material,
                           D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr,
                           const DirectX::XMMATRIX& world, bool ignoreDepth = false);

    bool IsSkinningAvailable() const { return m_pbrSkinnedRootSig != nullptr; }

    // 1 フレームの描画手順:
    //   BeginFrame(camera) → DrawMesh(...) を必要回数 → EndFrame()
    void BeginFrame(const Camera& camera);
#if ENGINE_MODEL_ANIMATION
    unsigned FrameSlot() const { return m_frameIndex; }
#endif
    // ignoreDepth=true で深度テストを無効化して描く（ウォールハックの実演用）。
    // 深度無視のメッシュは手前の物体を上書きするため、遮蔽物越しに見える。
    void DrawMesh(const Mesh& mesh, const DirectX::XMMATRIX& world,
                  bool ignoreDepth = false);

    // 敵専用の描画入口（ステージJ-01）。描画インターセプトの「継ぎ目」を経由する。
    // 既定（フック無し）では通常の深度(LESS)で描く＝壁で隠れる。ステージJ-02 で検証用
    // モジュールがこの継ぎ目を SetDrawInterceptor で差し替え、Enemy タグのドローだけ
    // 深度 Always にできる（本物に近いフック型ウォールハックの非汎用な再現）。
    void DrawEnemyTagged(const Mesh& mesh, const DirectX::XMMATRIX& world);

    // 敵専用の描画入口（スキン付き PBR 版。ステージ「敵キャラの Swat 化」で追加）。
    // DrawEnemyTagged と同じ継ぎ目（m_drawInterceptor）を通るので、ウォールハックの
    // 実演（cheat_tester --wallhack）と防御（門番・段[5]検知）は Swat 敵にもそのまま効く。
    // skinPaletteAddr は UploadSkinPaletteOnce が返したアドレス（1 キャラクター内の
    // 全パーツで使い回す。DrawPbrSkinnedMesh と同じ理由）。
    void DrawEnemyTaggedSkinned(const Mesh& mesh, const PbrMaterial& material,
                               D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr,
                               const DirectX::XMMATRIX& world);

    // 描画インターセプトの継ぎ目を差し替える（J-02：フック設置）。fn=nullptr で既定へ戻す。
    // ctx はインターセプタ側の状態（ホットキー等）を渡すための不透明ポインタ。
    // ※ tag は生の enum ではなく「マスクされ得るトークン」として渡す（J-04）。フックはこれを
    //   kEnemyTagPlain と照合して敵を判定するが、マスク ON だと一致しなくなる。
    // ※ material/texture/pbr/skinPaletteAddr は「敵キャラの Swat 化」で追加。フックは基本
    //   これらの中身を見ずにそのまま SubmitTaggedDraw へ転送するだけでよい（材質を問わず
    //   同じ深度すり替えロジックが効く設計）。
    using DrawInterceptor = void (*)(Renderer& r, const Mesh& mesh,
                                     const DirectX::XMMATRIX& world, uint32_t tagToken,
                                     MaterialType material, const Texture* texture,
                                     const PbrMaterial* pbr,
                                     D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr, void* ctx);
    void SetDrawInterceptor(DrawInterceptor fn, void* ctx);

    // インターセプタ（または既定経路）から呼ぶ実描画。タグトークンと深度モードを指定して submit。
    // トークンは内部で復号して DrawTag に戻す（J-04）。敵タグのとき実際に使った深度比較関数を
    // 記録する（段[5]検知器が GetLastEnemyDepthFunc で照合）。
    // ステージL-00: material / texture を追加した（既定値ありなので既存呼び出しは不変）。
    // テクスチャ付きの敵（L-01 以降で glTF モデルの敵を描くとき）も、この 1 本を通せば
    // 門番（ResolveDepthMode）と段[5]検知の記録が自動的に効く。
    // 「敵キャラの Swat 化」で pbr / skinPaletteAddr を追加（PbrSkinned 用。既定値ありなので
    // 既存呼び出しは不変）。
    void SubmitTaggedDraw(const Mesh& mesh, const DirectX::XMMATRIX& world,
                          uint32_t tagToken, DepthMode mode,
                          MaterialType material = MaterialType::Lit,
                          const Texture* texture = nullptr,
                          const PbrMaterial* pbr = nullptr,
                          D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr = 0);

    // ステージJ-04（防御②B：敵タグの暗号化マスク）。フックが受け取る「敵ドローの印」を
    // フレーム毎の鍵で XOR マスクする。マスク ON だと、素の敵トークン(kEnemyTagPlain)との
    // 単純比較でしか敵を識別しないフックは「どのドローが敵か」を特定できなくなる（＝透視できない）。
    // エンジンは鍵を知っているので DecodeTag で正しく復号し、自分の描画/防御は正常に行える。
    void SetDrawTagMask(bool on) { m_tagMaskOn = on; }

    // フック（検証用モジュール）が「素の敵タグ」を照合するための公開値（J-04 デモ用）。
    static constexpr uint32_t kEnemyTagPlain   = 0xE0E0E0E0u;
    static constexpr uint32_t kDefaultTagPlain = 0x0D0D0D0Du;

    // ステージJ-03（防御②A：事前 enforcement）。ON のとき、敵ドローの深度 ALWAYS を
    // 「サブミット直前」に拒絶して LESS へ戻す＝フックが ALWAYS を要求しても壁越しにさせない。
    // P 防御トグルに連動させる（ゲームが SetEnemyDepthGuard(m_defenseOn) を呼ぶ）。
    void SetEnemyDepthGuard(bool on) { m_enemyDepthGuard = on; }

    // フォグ（空気遠近）の ON/OFF。既定 ON。空を描く前に一時 OFF にして空はフォグさせない。
    void SetFog(bool on) { m_fogEnabled = on; }

    // ---- ステージL-03b: シャドウマップ ----------------------------------------
    // 影の ON/OFF。既定 ON。シャドウ用シェーダーが読めなかった場合は自動で OFF になる。
    void SetShadowEnabled(bool on) { m_shadowEnabled = on; }
    bool IsShadowEnabled() const { return m_shadowEnabled && m_shadowPSO != nullptr; }

    // 影を落とす範囲（ライトの正射影の半幅・メートル）。既定 35m。
    // 広げるほど広範囲に影が出るが、シャドウマップ 1 テクセルが担当する面積が増えて
    // 影の輪郭が粗くなる（解像度は固定 2048 のため）。マップの広さに合わせて調整する。
    void SetShadowExtent(float halfExtentMeters) { m_shadowHalfExtent = halfExtentMeters; }

    // 深度バイアス。大きくすると縞模様（シャドウアクネ）が消えるが、
    // 影が物体から離れて浮いて見える（ピーターパン現象）。既定 0.0015。
    void SetShadowBias(float bias) { m_shadowBias = bias; }

    // 直近の敵ドローで「フックが要求した」深度比較関数（門番による拒絶の“前”）。
    // GetLastEnemyDepthFunc()（拒絶“後”＝実際に使った値）と比べると、拒絶が起きたか分かる。
    D3D12_COMPARISON_FUNC GetRequestedEnemyDepthFunc() const { return m_lastEnemyDepthRequested; }

    // 敵メッシュを深度モード直指定で描く後方互換 API（フック非経由の直接描画）。
    //   wallhack=false → DepthMode::Normal（LESS）, wallhack=true → DepthMode::Always。
    void DrawEnemyMesh(const Mesh& mesh, const DirectX::XMMATRIX& world, bool wallhack);

    // 直近の敵ドローでバインドされた深度比較関数（検知器が LESS/LESS_EQUAL 逸脱を照合）。
    D3D12_COMPARISON_FUNC GetLastEnemyDepthFunc() const { return m_lastEnemyDepthFunc; }

    // 2D オーバーレイ（HUD）を描画する。頂点は NDC 座標・ライティングなし・深度無効。
    // 不透明物の後・EndFrame の前に呼ぶこと（最前面に重なる）。
    void DrawOverlay(const Mesh& mesh);

    void EndFrame();

    // 平行光源と環境光を設定する（任意。未設定なら下のデフォルト値を使う）。
    //   dir       : 光が進む向き（内部で正規化）
    //   color     : 光の色
    //   intensity : 強度
    //   ambient   : 環境光の色
    void SetDirectionalLight(const DirectX::XMFLOAT3& dir,
                             const DirectX::XMFLOAT3& color,
                             float intensity,
                             const DirectX::XMFLOAT3& ambient);

    // 画面の横/縦比（2D オーバーレイのアスペクト補正に使う）。
    float GetAspect() const {
        return (m_height > 0) ? static_cast<float>(m_width) / static_cast<float>(m_height)
                              : 16.0f / 9.0f;
    }

    // 終了時の GPU 完了待ち（リソース解放前に呼ぶ）
    void Shutdown();

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd);
    void CreateRenderTargets();
    void CreateDepthBuffer();
    void CreateRootSignature();
    void CreatePipelineState();
    void CreateConstantBuffers();
    void CreateSrvHeap();                // テクスチャ SRV 用の shader-visible ヒープ
    void CreatePbrRootSignature();       // L-02: CBV + マテリアル SRV×3 + シャドウ SRV
    void CreatePbrPipelineState();       // L-02: PBR PSO（深度 Normal/Always/Disabled）
    void CreateSkinnedRootSignature();   // L-04b: PBR ルートシグネチャ + ジョイント CBV(b1)
    void CreateSkinnedPipelineState();   // L-04b: スキン PSO（PbrSkinnedVS + 既存 PbrPS）
    void CreateShadowSkinnedRootSignature(); // L-04b: シャドウ用（CBV b0 + CBV b1）
    void CreateShadowSkinnedPipelineState(); // L-04b: シャドウ用スキン PSO（深度専用）
    void CreateSkinBuffers();            // L-04b: ジョイント行列パレット用アップロードバッファ
    // UploadSkinPaletteOnce（公開）の実体はここでなく Renderer.cpp 内の定義を参照。
    // 0 個やヒープ超過時は 0（無効値）を返す。
    void CreateDummyTextures();          // L-02: マテリアルの欠けた枠を埋める中立テクスチャ
    // SRV ヒープから連続した count 個のスロットを確保して先頭インデックスを返す。
    UINT AllocSrvSlots(UINT count);
    // 指定スロットに resource の SRV を書く（同じリソースを複数スロットから参照してよい）。
    void WriteSrvAt(ID3D12Resource* resource, UINT slot);

    void CreateShadowMap();              // L-03b: シャドウマップ本体 + DSV + SRV
    void CreateShadowRootSignature();    // L-03b: シャドウパス用（ルート CBV のみ）
    void CreateShadowPipelineState();    // L-03b: 深度専用 PSO（PS 無し）
    void CreateTextureRootSignature();   // CBV + SRVテーブル + 静的サンプラー
    void CreateTexturePipelineState();   // テクスチャ PSO（深度有効／無効）

    void MoveToNextFrame();
    void WaitForGpu();

    // ---- ステージL-03a: 描画のディファード化 ----------------------------------
    // Draw* は「その場で GPU 命令を積む」のをやめ、いったんこのリストに溜める。
    // EndFrame でまとめて再生（replay）することで、本描画の“前”に別パスを差し込めるようになる。
    // L-03b のシャドウマップは「同じ描画リストをライト視点でもう一度再生する」だけで実現でき、
    // ゲーム側のコード（World / 各 Mode）は一切変更しなくて済む。
    //
    // ※ world 行列は XMMATRIX（16 バイトアライン要求あり）ではなく XMFLOAT4X4 で持つ。
    //   std::vector の要素にアライン要求のある型を入れるのは事故のもとなので避ける。
    // ※ mesh / texture はポインタで持つ。ゲーム側がフレーム中それらを破棄しないことが前提
    //   （実際 World や各 Mode のメンバとして保持され続けるので満たされている）。
    struct DrawRecord {
        const Mesh*         mesh     = nullptr;
        DirectX::XMFLOAT4X4 world{};
        MaterialType        material = MaterialType::Lit;
        DepthMode           depth    = DepthMode::Normal;
        const Texture*      texture  = nullptr;  // Textured のときのみ
        const PbrMaterial*  pbr      = nullptr;  // Pbr / PbrSkinned のときのみ（L-02/L-04b）
        // PbrSkinned のときのみ有効。RecordDraw の時点でアップロード済みのジョイント行列
        // パレットの GPU アドレス（0 = 無効）。ポインタではなくアドレスで持つのは、
        // 呼び出し元の std::vector が Update() のたびに書き換わるため、記録時点で
        // アップロードを済ませて「値」を持ち回る必要があるから。
        D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr = 0;
        bool                isEnemy  = false;    // 段[5]検知・門番の記録用
    };

    // 描画を 1 件リストへ積む（PSO の存在確認と門番の適用はこの時点で済ませる）。
    void RecordDraw(const Mesh& mesh, const DirectX::XMMATRIX& world,
                    MaterialType material, DepthMode depth,
                    const Texture* texture, bool isEnemy,
                    const PbrMaterial* pbr = nullptr,
                    D3D12_GPU_VIRTUAL_ADDRESS skinPaletteAddr = 0);

    // ステージL-03b: シャドウパス。描画リストをライト視点でもう一度再生し、
    // 深度だけをシャドウマップへ書く。BeginMainPass の“前”に EndFrame から呼ぶ。
    void RenderShadowPass();

    // ライト視点のビュー×射影行列を計算する（BeginFrame でカメラ位置から毎フレーム更新）。
    void UpdateLightMatrix();

    // 本描画パスの開始（RT への遷移・クリア・ヒープバインド）。EndFrame から呼ぶ。
    void BeginMainPass();

    // 溜めた描画リストを 1 件ずつ GPU 命令に変換して積む。
    void ReplayDrawList();

    // DrawRecord 1 件を GPU 命令へ（ルートシグネチャ／PSO／定数バッファ／描画）。
    void EmitDraw(const DrawRecord& rec);

    // ルートシグネチャと PSO を呼び出し側で設定した後に、定数バッファ書き込み →
    // 頂点/インデックス設定 → 描画を行う。
    // texture != nullptr のとき SRV デスクリプタテーブル(param1)も束ねる（テクスチャ経路）。
    void WriteCbAndDraw(const Mesh& mesh, const DirectX::XMMATRIX& world,
                        const Texture* texture = nullptr,
                        const PbrMaterial* pbr = nullptr);

    // ---- ステージL-00: PSO キャッシュ ----------------------------------------
    // PSO は「マテリアル × 深度モード × スキニング有無 × ブレンド有無」で一意に決まる。
    // 32bit にパックしてキーにする。ビット割り当て（下位から）:
    //   [0..3]  DepthMode      （16 種まで）
    //   [4..11] MaterialType   （256 種まで）
    //   [12]    skinned        （L-04 スキンドアニメで使う）
    //   [13]    blend          （L-04/L-06 の半透明・加算合成で使う）
    // 予約ビットを先に切ってあるので、後段で種別が増えてもキー形式を壊さずに済む。
    static constexpr uint32_t MakePsoKey(MaterialType material, DepthMode depth,
                                         bool skinned = false, bool blend = false) {
        return (static_cast<uint32_t>(depth) & 0xFu)
             | ((static_cast<uint32_t>(material) & 0xFFu) << 4)
             | (skinned ? (1u << 12) : 0u)
             | (blend   ? (1u << 13) : 0u);
    }

    // 生成済み PSO をキャッシュへ登録する（CreatePipelineState 等から呼ぶ）。
    // ※ ここは private セクション内の ComPtr エイリアス宣言より前なので、
    //    エイリアスではなく完全修飾名で書く必要がある（クラス内でも型名は宣言順に依存する）。
    void RegisterPso(MaterialType material, DepthMode depth,
                     const Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso,
                     bool skinned = false, bool blend = false);

    // キャッシュから PSO を引く。未登録なら nullptr（呼び出し側で描画をスキップ）。
    ID3D12PipelineState* GetPso(MaterialType material, DepthMode depth,
                                bool skinned = false, bool blend = false) const;

    // 初期化の最後に呼び、必要な組み合わせが揃っているかを検査してログを出す。
    // 「PSO を増やしたのに敵用 ALWAYS 版を作り忘れて門番に穴が空く」事故を防ぐための番人。
    void ValidatePsoCache() const;

    // ---- ステージL-00/J-03: 深度モードの門番（一般化版）-----------------------
    // 敵タグのドローに対して「要求された深度モード」を検査し、防御 ON なら Normal 以外を
    // すべて拒絶する。要求値（拒絶前）を m_lastEnemyDepthRequested に記録するのもここ。
    // すべての描画入口がこの 1 箇所を通るようにしてあるので、経路が増えても漏れない。
    DepthMode ResolveDepthMode(DrawTag tag, DepthMode requested);

    // DepthMode → 検知器が照合する D3D12 の深度比較関数。
    // Disabled は「深度テスト無し＝全部通る」なので、意味としては ALWAYS と等価に報告する
    // （検知器は LESS/LESS_EQUAL からの逸脱を見るため、これで透視状態として拾える）。
    static D3D12_COMPARISON_FUNC DepthFuncOf(DepthMode mode) {
        return (mode == DepthMode::Normal) ? D3D12_COMPARISON_FUNC_LESS
                                           : D3D12_COMPARISON_FUNC_ALWAYS;
    }

    // J-04: タグの暗号化マスク。マスク ON のときフレーム鍵で XOR する。
    uint32_t EncodeTag(DrawTag tag) const {
        security::Measure measure(security::Work::TagMask,m_tagMaskOn);
        const uint32_t base = (tag == DrawTag::Enemy) ? kEnemyTagPlain : kDefaultTagPlain;
        return m_tagMaskOn ? (base ^ m_frameKey) : base;
    }
    DrawTag DecodeTag(uint32_t token) const {
        security::Measure measure(security::Work::TagMask,m_tagMaskOn);
        const uint32_t v = m_tagMaskOn ? (token ^ m_frameKey) : token;
        return (v == kEnemyTagPlain) ? DrawTag::Enemy : DrawTag::Default;
    }

    // 事前コンパイル済み .dxil を読み込む。無ければ空を返す（アサートしない）。
    static std::vector<uint8_t> LoadShaderBlob(const char* fileName);
    // .dxil が無い場合のフォールバック: 実行時に HLSL をコンパイルして DXBC を得る。
    static Microsoft::WRL::ComPtr<ID3DBlob> CompileHlsl(
        const char* hlslName, const char* entry, const char* target);
    // dxil を優先しつつ、無ければ実行時コンパイルへフォールバックする統合版。
    static std::vector<uint8_t> GetShaderBytecode(
        const char* dxilName, const char* hlslName,
        const char* entry, const char* target);

    static constexpr UINT kFrameCount = 3;      // トリプルバッファリング
    static constexpr UINT kMaxDrawsPerFrame = 512;   // 1 フレームに積める描画“件数”の上限
    // ステージL-03a: 定数バッファのスロット数は描画件数の 2 倍を確保する。
    // ディファード化により、同じ描画リストを「シャドウパス（ライト視点）」と
    // 「本描画パス（カメラ視点）」で 2 回再生する可能性があり、
    // それぞれが別の行列を持つ＝別スロットを消費するため。
    static constexpr UINT kMaxCbSlotsPerFrame = kMaxDrawsPerFrame * 2;
    static constexpr UINT kCbStride = sizeof(SceneConstants); // 256B

    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    int  m_width  = 0;
    int  m_height = 0;

    ComPtr<IDXGIFactory6>        m_factory;
    ComPtr<ID3D12Device>         m_device;
    ComPtr<ID3D12CommandQueue>   m_commandQueue;
    ComPtr<IDXGISwapChain3>      m_swapChain;

    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    UINT m_rtvDescriptorSize = 0;

    ComPtr<ID3D12Resource>          m_renderTargets[kFrameCount];
    ComPtr<ID3D12CommandAllocator>  m_commandAllocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    ComPtr<ID3D12Resource> m_depthBuffer;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;        // 通常（深度テスト有効・LESS）
    ComPtr<ID3D12PipelineState> m_pipelineStateNoDepth; // 深度無効（既存の透視/エフェクト用）
    ComPtr<ID3D12PipelineState> m_enemyDepthAlwaysPSO;  // タイプA[5]再現（DepthFunc=ALWAYS）
    ComPtr<ID3D12PipelineState> m_overlayPSO;           // 2D HUD（深度無効・ライティングなし）

    // ステージL-00: 上記の個別メンバは「生成時の保持先」として残し、描画時の参照は
    // すべてこのキャッシュ経由にした（同じ COM オブジェクトを指すので二重生成はしない）。
    // L-02 以降で PBR / スキンド / 半透明の PSO が増えても、描画側のコードは
    // GetPso(material, depth) のままで済む。
    std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> m_psoCache;

    // 段[5]検知器用: 直近の敵ドローでバインドした深度比較関数（門番の“後”＝実際に使った値）。
    // SubmitTaggedDraw が Enemy タグのとき更新し、FpsMode の検知器がフレーム末に照合する。
    D3D12_COMPARISON_FUNC m_lastEnemyDepthFunc = D3D12_COMPARISON_FUNC_LESS;
    // フックが要求した深度比較関数（門番の“前”）。拒絶が起きたかの判定に使う。
    D3D12_COMPARISON_FUNC m_lastEnemyDepthRequested = D3D12_COMPARISON_FUNC_LESS;
    // J-03 事前 enforcement の ON/OFF（P 防御に連動）。
    bool m_enemyDepthGuard = false;

    // 描画インターセプトの継ぎ目（ステージJ-02 でフックされる差し替え点）。
    // null = 既定（エンジンの通常描画）。SetDrawInterceptor で検証用モジュールが差し替える。
    DrawInterceptor m_drawInterceptor    = nullptr;
    void*           m_drawInterceptorCtx = nullptr;

    // J-04: 敵タグのマスク。m_tagMaskOn のときフレーム鍵で XOR してフックの識別を妨げる。
    bool     m_tagMaskOn = false;
    uint32_t m_frameKey  = 0x9E3779B9u; // BeginFrame で毎フレーム更新（非0 を保証）
    uint32_t m_rngState  = 0xC2B2AE35u; // フレーム鍵生成用の内部 RNG 状態

    // フォグ（空気遠近）。DrawMesh/DrawTexturedMesh が cameraPos.w に反映し、PS が距離減衰させる。
    bool m_fogEnabled = true;

    // ---- ステージL-03b: シャドウマップ ----------------------------------------
    // 解像度 2048×2048 の根拠: 影を落とす範囲を 70m 四方（m_shadowHalfExtent=35 の 2 倍）
    // とすると 1 テクセル ≒ 3.4cm。キャラの足元の影が破綻しない粒度で、
    // かつ D32_FLOAT で 16MB に収まる（毎フレーム全面を描き直すので帯域も許容範囲）。
    static constexpr UINT kShadowMapSize = 2048;

    ComPtr<ID3D12Resource>      m_shadowMap;        // R32_TYPELESS（DSV=D32_FLOAT / SRV=R32_FLOAT）
    ComPtr<ID3D12RootSignature> m_shadowRootSig;    // ルート CBV(b0) のみ
    ComPtr<ID3D12PipelineState> m_shadowPSO;        // 深度専用（PS 無し）
    UINT m_shadowDsvIndex = 1;                      // m_dsvHeap 内の位置（0 = 本描画の深度）
    UINT m_shadowSrvIndex = 0;                      // m_srvHeap 内の位置（Initialize で先頭を確保）

    bool  m_shadowEnabled    = true;
    float m_shadowHalfExtent = 35.0f;   // ライト正射影の半幅（メートル）
    float m_shadowBias       = 0.0015f; // 深度バイアス
    DirectX::XMFLOAT4X4 m_lightViewProj{}; // BeginFrame で更新

    // テクスチャ描画（PROMPT-G-01）
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;             // shader-visible CBV_SRV_UAV（SRV 群）
    UINT m_srvDescriptorSize = 0;
    UINT m_srvCount          = 0;                        // 発行済み SRV 数（＝次の空きインデックス）
    // SRV ヒープ容量。ステージL-02 で 128 → 512 に拡張した。
    // 内訳の根拠:
    //   1  … シャドウマップ
    //   2  … PBR のダミーテクスチャ（白 / 平坦法線）
    //   ~55… トランプ絵柄（52 枚 + 裏 + ジョーカー）
    //   ~5 … 地面 / レンガ壁 / 木箱 など
    //   3×N… PBR マテリアル（1 マテリアルにつき baseColor/normal/ORM の 3 スロット）
    // 外部の CC0 モデルはマテリアルを 20 個以上持つことがあり（20×3=60）、
    // 128 だと足りなくなる。デスクリプタ 512 個でも数十 KB なので余裕を持たせる。
    static constexpr UINT kMaxTextures = 512;
    ComPtr<ID3D12RootSignature> m_texRootSig;           // CBV(b0) + SRVテーブル(t0) + 静的サンプラー(s0)
    ComPtr<ID3D12PipelineState> m_texturePSO;           // テクスチャ（深度有効）
    ComPtr<ID3D12PipelineState> m_texturePSONoDepth;    // テクスチャ（深度無効）
    // ステージL-00: テクスチャ経路の「敵用 ALWAYS 版」。L-01 以降でテクスチャ付きの敵
    // モデルを描くようになったとき、これが無いと門番が守るべき対象を作れない（＝穴）。
    // 先に用意しておくことで、マテリアルが増えても防御の網羅性が保たれる。
    ComPtr<ID3D12PipelineState> m_texturePSOAlways;     // テクスチャ（深度 ALWAYS）

    // ---- ステージL-02: PBR ----
    ComPtr<ID3D12RootSignature> m_pbrRootSig;       // CBV(b0) + SRV表(t0-t2) + SRV表(t3) + s0/s1
    ComPtr<ID3D12PipelineState> m_pbrPSO;           // 深度 LESS
    ComPtr<ID3D12PipelineState> m_pbrPSONoDepth;    // 深度無効
    ComPtr<ID3D12PipelineState> m_pbrPSOAlways;     // 深度 ALWAYS（敵の門番対象）
    Texture m_dummyWhite;   // baseColor / ORM の欠け埋め（AO=1・粗さ1・金属1）
    Texture m_dummyNormal;  // 平坦な法線マップ (128,128,255)

    // ---- ステージL-04b: GPU スキニング ----
    // PbrSkinned はマテリアル(t0-t2)+シャドウ(t3) は PBR と同じだが、追加でジョイント行列
    // パレットをルート CBV(b1) として渡す必要があるため、専用のルートシグネチャを持つ。
    ComPtr<ID3D12RootSignature> m_pbrSkinnedRootSig;
    ComPtr<ID3D12PipelineState> m_pbrSkinnedPSO;
    ComPtr<ID3D12PipelineState> m_pbrSkinnedPSONoDepth;
    ComPtr<ID3D12PipelineState> m_pbrSkinnedPSOAlways;

    // シャドウパス用のスキン版（CBV b0 + CBV b1 のみ。テクスチャ／サンプラー不要）。
    ComPtr<ID3D12RootSignature> m_shadowSkinnedRootSig;
    ComPtr<ID3D12PipelineState> m_shadowSkinnedPSO;

    // ジョイント行列パレット専用のアップロードバッファ（SceneConstants とは別枠）。
    // 1 スロット = kMaxJoints(128) × 64B(転置済み float4x4) = 8192B。256B 境界に揃っている。
    // スロット数の根拠: 1 フレームに同時描画するスキン付きキャラの想定上限（敵/hider の数）。
    static constexpr UINT kSkinPaletteStride    = kMaxJoints * 64u; // 8192B
    static constexpr UINT kMaxSkinnedDrawsPerFrame = 32;
    ComPtr<ID3D12Resource> m_skinBuffers[kFrameCount];
    uint8_t*               m_skinMapped[kFrameCount] = {};
    UINT                    m_skinSlot = 0;   // このフレームで消費したパレットスロット数

    // フレームごとの定数バッファ（永続マップ）。1 フレームで kMaxDrawsPerFrame 個まで。
    ComPtr<ID3D12Resource> m_constantBuffers[kFrameCount];
    uint8_t*               m_cbMapped[kFrameCount] = {};
    UINT                   m_drawCount = 0;   // このフレームで積んだ描画“件数”
    UINT                   m_cbSlot    = 0;   // このフレームで消費した定数バッファスロット数

    // ステージL-03a: このフレームに積まれた描画（EndFrame でまとめて再生する）。
    // capacity は Initialize で予約し、フレーム中の再確保（＝アロケーション）を避ける。
    std::vector<DrawRecord> m_drawList;

    // フェンス同期
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValues[kFrameCount] = {};
    UINT   m_frameIndex = 0;

    // BeginFrame で確定した行列を DrawMesh まで持ち回る
    DirectX::XMMATRIX m_viewMatrix{};
    DirectX::XMMATRIX m_projMatrix{};

    // ライティング（DrawMesh で定数バッファに書き込む）。デフォルトは斜め上からの白色光。
    DirectX::XMFLOAT4 m_lightDir{ 0.4f, -1.0f, 0.3f, 0.0f };   // 後で正規化して使う
    DirectX::XMFLOAT4 m_lightColor{ 1.0f, 0.98f, 0.95f, 1.0f }; // w=強度
    DirectX::XMFLOAT4 m_ambient{ 0.25f, 0.25f, 0.30f, 0.0f };
    DirectX::XMFLOAT3 m_cameraPos{ 0.0f, 0.0f, 0.0f };          // BeginFrame で更新

    bool m_initialized = false;
};
