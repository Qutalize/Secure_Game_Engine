#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <vector>

#include <DirectXMath.h>

#include "game/IGameMode.h"
#include "game/SecurityControls.h"
#include "game/Player.h"
#include "game/GltfProp.h"                       // 一人称ビューモデル（銃）の読込/描画
#include "game/SecretSignatures.h"               // 描画制御ブロック（深度帯の CPU 代役）
#include "engine/renderer/Camera.h"
#include "engine/renderer/Renderer.h"   // Mesh（敵メッシュを値で持つため完全型が必要）
#include "engine/renderer/Animation.h"  // AnimationPlayer（敵の Swat 化。個体ごとに独立した再生状態）
#include "engine/memory/ShuffleAllocator.h"
#include "engine/security/IntegrityWatchdog.h"   // 改ざん検知（防御①）
#include "engine/network/Packets.h"
#include "engine/audio/AudioPlayer.h"   // 発砲音

// 前方宣言（重いヘッダの取り込みを cpp 側へ寄せる）
class LinearAllocator;
class World;
// class CheatModule; // [旧チート・無効化] 協調型 DLL ローダーは撤去（下記 m_cheat 参照）

// 敵（標的）。ローカル完結の素朴実装。
// SECURITY NOTE: 体力・座標はすべてクライアント側に存在する。これは後の
// ステージC（チート）でメモリ改ざん/ESP の対象になる前提の設計である。
//
// PROMPT-G-02: 巡回(Patrol)→索敵で追跡(Chase) の状態機械と、遠距離ヒットスキャン
// 射撃・向き(yaw)を追加。撃ち合いに手応えを出す。
struct Enemy {
    enum class State { Patrol, Chase };

    DirectX::XMFLOAT3 spawnPos{};        // リスポーン位置（足元）
    DirectX::XMFLOAT3 position{};        // 現在位置（足元, y=0）
    DirectX::XMFLOAT3 patrolA{};         // 巡回の端点 A
    DirectX::XMFLOAT3 patrolB{};         // 巡回の端点 B
    bool   toB   = true;                 // 現在 B へ向かっているか（巡回のピンポン）
    State  state = State::Patrol;
    float  yaw   = 0.0f;                 // 向き（描画・射撃方向）
    float  walkPhase = 0.0f;             // 歩行アニメの位相（移動量で進む）
    float  health = 100.0f;
    bool   alive  = true;
    float  respawnTimer = 0.0f;          // 撃破後の再出現までの残り秒
    float  fireCooldown = 0.0f;          // 次の射撃までの残り秒
    float  hitFlash     = 0.0f;          // 被弾フラッシュの残り秒（>0 で白く光る）

    // 敵の Swat 化: 個体ごとに独立したアニメーション再生状態。同じ GltfModel（World が
    // 所有）を全敵で共有しつつ、再生位置/クリップだけ個体別に持つ（Bind は Init で一度）。
    // World::HasAnimatedCharacter() が false（glTF 未読込・PBR/スキニング非対応）の
    // ときは Bind されないままで、Render 側が従来の箱モデルへフォールバックする。
    AnimationPlayer anim;
};

// 既存の FPS ゲーム（World / Player / AABB 当たり判定 / 座標同期 /
// ウォールハック内蔵実演）を IGameMode 実装としてまとめたもの（PROMPT-V3-01）。
//
// ※ 中身のゲームロジックは PROMPT-08 の Main.cpp 実装をそのまま移植している。
//    挙動を変えないことが本 PROMPT の完了条件のため、計算式・定数は不変。
class FpsMode : public IGameMode {
public:
    // コンストラクタ・デストラクタとも cpp 側で定義する。
    // unique_ptr<LinearAllocator> が不完全型のままヘッダで実体化されると
    // 「can't delete an incomplete type」になるため、LinearAllocator の
    // 完全定義が見える FpsMode.cpp で =default する（ctor も例外時の破棄で必要）。
    FpsMode();
    ~FpsMode() override;

    bool Init(const GameContext& ctx) override;
    void Update(float dt, const Input& input) override;
    void Render(Renderer& renderer) override;
    void Shutdown() override;
    const char* GetName() const override { return "fps"; }

private:
    security::Controls m_security;
    // 共有リソース（GameContext から借用。所有しない）
    Renderer*       m_renderer = nullptr;
    NetworkManager* m_net      = nullptr;
    Window*         m_window   = nullptr;

    // ゲームオブジェクト（PROMPT-05/08 のアロケータで確保）
    std::unique_ptr<LinearAllocator>  m_gameMemory;   // World 用アリーナ
    std::unique_ptr<ShuffleAllocator> m_playerAlloc;  // Player 用（毎フレームシャッフル）
    World*         m_world = nullptr;  // m_gameMemory 上にプレースメント new
    Handle<Player> m_player;          // ShuffleAllocator のハンドル経由で参照

    Camera m_camera;

    // --- シューティングバトル（PROMPT-V3-03）---
    std::vector<Enemy> m_enemies;     // 敵リスト
    // 分節した人型（ローポリ・スタイライズド）。胴＝静的、腕/脚＝関節ピボットで歩行スイング。
    Mesh               m_guardBodyMesh;// 胴体+頭+ヘルメット+バイザー+銃（静的）
    Mesh               m_guardArmMesh; // 腕1本（肩ピボット。上腕+前腕+手。脚と逆位相で振る）
    Mesh               m_guardLegMesh; // 脚1本（股関節ピボット。腿+脛+ブーツ。左右で位相反転）
    // Mesh            m_enemyEspMesh; // [旧チート・無効化] 専用 ESP 箱は不要になった。
    //   タイプA では敵メッシュ自体を深度ALWAYS(DrawEnemyMesh)で描いて壁越しにする。
    Mesh               m_crosshair;   // 画面中央の照準（2D HUD）
    bool               m_prevFire = false; // 射撃の押下エッジ検出用
    int                m_kills    = 0;      // 撃破数（HUD・有利さの指標）

    // 一人称ビューモデル（銃。Poly Universal Pack の AK-47 を Blender で glTF 化）。
    // 読み込み失敗時は IsLoaded()==false のままになり、Render 側が描画をスキップする
    // （視界に何も浮かばないだけ＝安全側フォールバック。L-02 の IsPbrAvailable() と同じ方針）。
    GltfProp           m_viewGun;

    // スポーン保護: 復活直後の一定時間ダメージ無効（リスポーン直後の即死ループ防止）。
    float m_spawnProtect = 0.0f;

    // クリア状態（撃破数が目標に達したら勝利。再プレイ/終了の画面を出す）。
    bool  m_cleared    = false;
    bool  m_prevRetry  = false; // R / △ の押下エッジ
    Mesh  m_clearOverlay;       // 「CLEAR」画面（2D オーバーレイ）
    void  ResetEncounter();     // 敵・プレイヤーを初期状態へ戻す（開始/死亡/再プレイ）

    // 射撃フィードバック（弾道・マズルフラッシュ・被弾フラッシュ・発砲音）
    Mesh  m_tracerMesh;    // 弾道（+Z 方向の細い単位箱, 黄）
    Mesh  m_muzzleMesh;    // マズルフラッシュ（小さい明色の箱）
    Mesh  m_hitFlashMesh;  // 敵の被弾フラッシュ（白い箱, 足元原点）
    DirectX::XMFLOAT3 m_tracerStart{};
    DirectX::XMFLOAT3 m_tracerDir{ 0.0f, 0.0f, 1.0f };
    float m_tracerLen   = 0.0f;
    float m_tracerTimer = 0.0f;   // >0 の間だけ弾道/マズルを描く
    // 音は AudioPlayer::Get()（シングルトン）で鳴らす（メンバは持たない）

    // 射撃処理（左クリック時に呼ぶ）。カメラ前方へレイを飛ばし最近接の敵に命中判定。
    void FireShot();
    void UpdateHud();   // ウィンドウタイトルに HP・撃破数・防御状態を表示

    // 他プレイヤー座標（受信して保持。描画は将来対応）
    std::unordered_map<uint32_t, PlayerStatePayload> m_remotePlayers;

    // [旧チート・無効化] 協調型の外部チートDLLローダー（HostState を手渡す方式）。
    //   ゲームがチートに秘匿情報を手渡す構図が非現実的だったため撤去。代わりに
    //   ゲームはチートを一切知らず、外部 cheat_tester がメモリを自力走査して下記
    //   m_renderCtrl の depthCheatFlag を書き換える（typeA_wallhack_explained.md 準拠）。
    // CheatModule* m_cheat = nullptr;

    // --- タイプA ウォールハックの「深度制御帯」（段[5]の CPU 側代役）と防御① ---
    // depthCheatFlag を持つ署名付きブロックを ShuffleAllocator 上に確保する。ゲームは
    // 毎フレームこのフラグを読んで敵を通常(LESS)/ウォールハック(ALWAYS)で描くだけ。
    // 外部 cheat_tester が署名走査で発見し WriteProcessMemory で書き換える＝チート発動。
    ShuffleAllocator                       m_ctrlAlloc{ 64, 8 };
    Handle<secrets::RenderControlBlock>    m_renderCtrl;

    // 段[5]検知器の状態（状態変化のエッジでのみログ）:
    //   0=正常 / 1=ウォールハック成立(実際に ALWAYS が使われた) / 2=フックの ALWAYS 要求を門番が拒絶
    int  m_wallhackState = 0;
    void DetectDepthTamper(Renderer& renderer);   // フレーム末の深度ステート整合性検証

    // 個別設定と入力状態は m_security が持つ。

    // ※ ウォッチドッグはメンバ宣言の最後＝最初に破棄（監視スレッドを join してから
    //   m_ctrlAlloc / m_renderCtrl が壊れる順にする。CardMode と同じ寿命規約）。
    IntegrityWatchdog                      m_watchdog;
    int                                    m_ctrlToken = -1;
};
