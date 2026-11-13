#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <memory>
#include "engine/network/CardClient.h"

#include "game/IGameMode.h"
#include "game/SecurityControls.h"
#include "game/SecretSignatures.h"       // 伏せ札の秘匿ブロック（チート検証用署名付き）
#include "engine/renderer/Camera.h"
#include "engine/renderer/Renderer.h"   // Mesh を値で持つため完全型が必要
#include "engine/memory/ShuffleAllocator.h"      // 秘匿ブロックを毎フレーム移動（難読化）
#include "engine/security/IntegrityWatchdog.h"   // 改ざん検知（防御①）

class CheatModule; // 外部チートDLLローダー（PROMPT-H。借用）

// トランプ 1 枚。rank 0..12（2,3,…,10,J,Q,K,A）, suit 0..3（H,D,C,S）。
// ジョーカーは rank/suit を持たない特別札（isJoker=true）。ババ抜きの「ババ」。
struct Card {
    uint8_t rank   = 0;
    uint8_t suit   = 0;
    bool    isJoker = false;
};

// カードゲームモード（PROMPT-V3-04 改 / ステージB）。
// ★本物のババ抜き（Player vs CPU）。同じランクのペアを捨て合い、最後にジョーカー
//   （ババ）を持っていた方が負け。プレイヤーは相手の手札から1枚引いてペアを狙う。
//
// SECURITY NOTE: オフライン版のみ全手札と走査用ブロックを持つ。
// オンライン版はサーバーの決定を表示し、自分の札と相手の枚数だけを保持する。
class CardMode : public IGameMode {
public:
    explicit CardMode(cardnet::Options options={}):m_remoteOptions(std::move(options)) {}
    bool Init(const GameContext& ctx) override;
    void Update(float dt, const Input& input) override;
    void Render(Renderer& renderer) override;
    void Shutdown() override;
    const char* GetName() const override { return "card"; }

private:
    static constexpr int kNumPlayers = 4;   // 0=あなた, 1..3=CPU（4人ババ抜き）

    void UpdateRemote(const Input&);
    cardnet::Options m_remoteOptions;
    std::unique_ptr<cardnet::Client> m_remote;
    cardnet::View m_remoteView;
    std::array<int,4> m_remoteCounts{};
    uint64_t m_remoteRevision=~uint64_t(0);
    bool m_remoteReady=false;
    Mesh m_waitOverlay;
    uint32_t m_autoChoice=0;

    void Deal();                                   // 山札を配って新しい1ゲームを開始
    void RemovePairs(std::vector<Card>& hand);     // 手札から同ランクのペアを全部捨てる
    int  NextActive(int from) const;               // from の次で手札が残っているプレイヤー（-1=無し）
    int  ActiveCount() const;                      // 手札が残っているプレイヤー数
    void DoDraw(int drawer, int victim, int idx);  // drawer が victim の idx 枚目を引く（引いてペアで捨てる）
    void AdvanceTurn();                            // 手番を次の生存プレイヤーへ進める
    void CheckGameOver();                          // 残り1人（ババ持ち）で決着
    void RefreshSecret();                          // 「引く相手」の手札を署名ブロックへ反映＋監視基準更新
    void UpdateHud();                              // ウィンドウタイトルに進行/勝敗を表示

    security::Controls m_security;
    Renderer* m_renderer = nullptr;  // 借用
    Window*   m_window   = nullptr;   // 借用（HUD用）
    Camera    m_camera;              // テーブルを見下ろす固定視点

    // 4人分の手札。index 0=あなた（表向き）, 1..3=CPU（伏せ）。
    // 本式ババ抜き: 引いた札が手札の同ランクと揃った時だけペアで捨てる（DoDraw 内）。
    std::array<std::vector<Card>, kNumPlayers> m_hands;
    std::array<bool, kNumPlayers>              m_finished{}; // 手札を出し切った＝上がり（安全）
    int   m_discardPairs = 0;        // これまでに捨てたペア総数（HUD用）

    int   m_turn    = 0;             // 現在の手番プレイヤー（0=あなた）
    int   m_victim  = 1;             // 現在の手番プレイヤーが引く相手
    int   m_selIndex = 0;            // あなたが狙っている victim 手札の位置
    float m_cpuTimer = 0.0f;         // CPU が「考える」演出の残り時間（秒）
    bool  m_gameOver = false;
    int   m_loser    = -1;           // ババ（ジョーカー）を持って負けたプレイヤー（-1=未決）

    // 透視チートは base には無い（外部 cheat_tester が担う）。
    CheatModule* m_cheat = nullptr; // 借用（現在は未使用）

    // オフラインの独立設定は m_security、オンラインはサーバー権威が必須。

    // 入力のエッジ検出
    bool m_prevLeft  = false; // A
    bool m_prevRight = false; // D
    bool m_prevDraw  = false; // Space（引く / 新ゲーム）

    // 描画（PROMPT-G-01: テクスチャ化）。カード/テーブルは UV 付きの共有クアッド
    // 1 枚に、差し替えるテクスチャで表現する（数字・スート絵柄を表示）。
    Mesh m_cardQuad;      // カード 1 枚分のテクスチャ付きクアッド（全カード共用）
    Mesh m_tableQuad;     // テーブル用テクスチャ付きクアッド
    Mesh m_highlightMesh; // 選択枠（頂点色。テクスチャなし＝DrawMesh で描く）
    Mesh m_winOverlay;    // 「YOU WIN」クリア画面（2D）
    Mesh m_loseOverlay;   // 「YOU LOSE」クリア画面（2D）
    std::array<Texture, 52> m_faceTex;  // 表 52 種（suit*13+rank）
    Texture m_backTex;    // 裏面
    Texture m_jokerTex;   // ジョーカー（ババ）
    Texture m_feltTex;    // カジノ卓フェルト

    // V3-07（ステージD）の秘匿ブロック＝チート検証ツール互換＋難読化＋改ざん検知。
    // CPU 手札の先頭数枚を署名付きブロックに反映し、ShuffleAllocator で毎フレーム移動。
    // ※ ウォッチドッグはメンバ宣言の最後＝最初に破棄（監視スレッドを join してから
    //   m_secretAlloc / m_opponent が壊れる順）。
    ShuffleAllocator                 m_secretAlloc{ 256, 16 };
    Handle<secrets::CardSecretBlock> m_opponent;
    IntegrityWatchdog                m_watchdog;
    int                              m_secretToken = -1;
};
