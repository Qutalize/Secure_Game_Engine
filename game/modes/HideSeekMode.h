#pragma once

#include <vector>

#include <DirectXMath.h>

#include "game/IGameMode.h"
#include "game/SecurityControls.h"
#include "game/Player.h"
#include "game/World.h"
#include "game/SecretSignatures.h"       // hider 座標の秘匿ブロック（チート検証用署名付き）
#include "engine/renderer/Camera.h"
#include "engine/renderer/Renderer.h"
#include "engine/renderer/Animation.h"            // AnimationPlayer（hider の Swat 化）
#include "engine/memory/ShuffleAllocator.h"      // 秘匿ブロックを毎フレーム移動（難読化）
#include "engine/security/IntegrityWatchdog.h"   // 改ざん検知（防御①）

class CheatModule; // 外部チートDLLローダー（PROMPT-H。借用）

// かくれんぼモード（PROMPT-V3-05 / ステージB のローカル素朴版）。
// プレイヤー=探す側(seeker)。障害物の陰に隠れた hider 全員を見つけるまでの
// 「タイム」と「移動距離」を競うスコアゲーム。
//
// SECURITY NOTE: 全 hider の座標がクライアント側メモリ（m_hiders）に存在する素朴実装。
// 本来 seeker が知ってはいけない「まだ見つけていない hider の位置」もメモリにある。
// これはステージC で ESP（壁越しの位置透視）／ウォールハックの対象になる前提であり、
// ステージF（サーバー権威）の「視界外 hider の座標を送らない」設計と対比する。
class HideSeekMode : public IGameMode {
public:
    bool Init(const GameContext& ctx) override;
    void Update(float dt, const Input& input) override;
    void Render(Renderer& renderer) override;
    void Shutdown() override;
    const char* GetName() const override { return "hide"; }

private:
    // 隠れる側。PROMPT-G-03 で徘徊＋逃走の AI 移動、I-03 で歩行アニメ＋発見リアクション。
    struct Hider {
        DirectX::XMFLOAT3 position{};    // 足元 y=0
        DirectX::XMFLOAT3 target{};      // 徘徊の目標点
        float wanderTimer = 0.0f;        // 次に目標を選び直すまでの残り秒
        float yaw = 0.0f;                // 向き（描画用）
        float walkPhase = 0.0f;          // 歩行アニメの位相（移動量で進む）
        float foundPop  = 0.0f;          // 発見リアクション（>0 で軽く跳ねる）
        bool  found = false;

        // hider の Swat 化: 個体ごとに独立したアニメーション再生状態（FpsMode::Enemy と同じ
        // 考え方）。World::HasAnimatedCharacter() が false のときは未 Bind のままで、
        // Render 側が従来の箱組み立てモデルへフォールバックする。
        AnimationPlayer anim;
    };

    void Reset();           // ゲームを初期化（タイム/距離リセット・全員未発見）
    void UpdateHud();        // ウィンドウタイトルに進捗を出す（簡易HUD）

    security::Controls m_security;
    Renderer* m_renderer = nullptr;  // 借用
    Window*   m_window   = nullptr;   // 借用（カーソルロック・HUD用）

    World  m_world;   // 床＋障害物（隠れ場所）。FpsMode と同じアリーナを流用
    Player m_player;  // 探す側（POD。座標は秘匿対象ではないので Shuffle 不要）
    Camera m_camera;

    std::vector<Hider> m_hiders;   // 隠れている全員（座標がメモリに存在＝ESP対象）

    // 全 hider 座標を署名付きブロックにも保持（m_hiders と同じ値）。
    // PROMPT-V3-07（ステージD）で固定メンバから ShuffleAllocator のスロットへ移し、
    // 毎フレーム別アドレスへ動かす（難読化）。値はハンドル経由でのみ触る。
    ShuffleAllocator                  m_secretAlloc{ 256, 16 };
    Handle<secrets::HiderSecretBlock> m_hider;     // 全 hider 座標ブロックのハンドル

    // 透視（ウォールハック）用の深度制御帯。FPS と同じ署名(kRenderCtrlMagic)。外部
    // cheat_tester --wallhack が走査で発見し depthCheatFlag=1 に書換＝hider を壁越し描画。
    Handle<secrets::RenderControlBlock> m_renderCtrl;

    Mesh m_hiderBodyMesh;          // 未発見 hider の胴+頭+帽子+顔（黄。腕/脚は別）
    Mesh m_hiderFoundBodyMesh;     // 発見済み hider の胴（緑）
    Mesh m_hiderArmMesh;           // hider の腕1本（肩ピボット・脚と逆位相で振る）
    Mesh m_hiderLegMesh;           // hider の脚1本（股関節ピボット・腿+脛+靴）
    Mesh m_hiderEspMesh;           // ESP（ウォールハック）用の赤い胴（壁越し透視で描く）
    Mesh m_crosshair;              // 画面中央の照準（2D HUD）
    Mesh m_clearOverlay;           // 「CLEAR」画面（2D。全員発見時に表示）

    // スコア
    float m_elapsed   = 0.0f;      // 経過タイム（秒）
    float m_distance  = 0.0f;      // 累積移動距離（m）
    int   m_foundCount = 0;
    bool  m_finished  = false;
    DirectX::XMFLOAT3 m_prevPos{}; // 移動距離計算用

    bool m_prevRestart = false;    // R のエッジ検出
    bool m_prevTamper  = false;    // T（改ざん実演）のエッジ検出

    // 個別設定と入力状態は m_security が持つ。

    // 透視 ESP は base には無い（外部 game_cheat.dll が担う）。未ロードなら効かない。
    CheatModule* m_cheat = nullptr; // 借用（所有は Main）

    // 整合性ウォッチドッグ（防御①）。hider 座標は不変なので、Init で一度
    // Commit した基準値からの変化＝改ざんとして警告する。
    // ※ 最後に宣言＝最初に破棄。監視スレッドを join してから m_secretAlloc /
    //   m_hider（Provider の参照先）が壊れる。
    IntegrityWatchdog m_watchdog;
    int               m_secretToken = -1;  // m_hider ブロックの監視トークン
    int               m_ctrlToken   = -1;  // m_renderCtrl（深度制御帯）の監視トークン
};
