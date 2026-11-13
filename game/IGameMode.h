#pragma once
#include "engine/security/SecurityPolicy.h"

// ゲームモード抽象化（PROMPT-V3-01）。
// V3 の3方針（描画向上 / サーバー権威 / 複数モード）すべての土台。
// FPS・カード・鬼ごっこなど各モードはこのインターフェースを実装し、
// GameModeManager がこのインターフェース越しに現在のモードを駆動する。

// 前方宣言（循環インクルード回避・コンパイル時間短縮。CLAUDE.md コーディング規約）
class Renderer;
class NetworkManager;
class Window;
class Input;
class CheatModule;

// ゲームモードが初期化時に受け取る共有リソース。
//
// 仕様(PROMPT-V3-01)の Init() は引数なしだが、実際には
//   ・GPU メッシュ生成        → Renderer
//   ・座標/状態のサーバー同期  → NetworkManager
//   ・FPS 視点のカーソルロック → Window
// が必要になる。これらを毎フレームではなく初期化時にまとめて渡すため、
// GameContext を導入した（最善方向への修正。所有権は Main 側にあり、
// モードはポインタを借用するだけ）。
struct GameContext {
    security::Policy security;
    Renderer*       renderer = nullptr;
    NetworkManager* net      = nullptr;
    Window*         window   = nullptr;
    // 外部チートDLLのローダー（PROMPT-H）。常に有効なポインタだが、`--cheat` 未指定や
    // DLL 不在なら IsLoaded()==false＝チートは効かない。各モードは借用するだけ。
    CheatModule*    cheat    = nullptr;
};

// すべてのゲームモードが実装する純粋仮想インターフェース。
class IGameMode {
public:
    virtual ~IGameMode() = default;

    // 初期化。メッシュ生成・初期状態の構築を行う。成功で true。
    virtual bool Init(const GameContext& ctx) = 0;

    // 毎フレームの更新（入力処理・移動・当たり判定・ネットワーク同期など）。
    virtual void Update(float dt, const Input& input) = 0;

    // 描画。renderer.BeginFrame(...) 〜 EndFrame() までモード自身が完結させる。
    virtual void Render(Renderer& renderer) = 0;

    // 後始末。プレースメント new したオブジェクトの破棄など
    //（GPU 完了待ちは Main 側が renderer.Shutdown() で行う前提）。
    virtual void Shutdown() = 0;

    // モード名（--mode 引数・ログ用）。例: "fps"
    virtual const char* GetName() const = 0;
};
