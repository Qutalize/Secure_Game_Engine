#pragma once

#include "game/CheatBridge.h"

// ★[非推奨・無効化] 協調型チート DLL ローダー。もう誰からも使われていません
//   （Main.cpp / 各モードの参照はコメントアウト済み。ファイルは参照用に残置）。
//   ゲームが自分でチート DLL を読み込む構図は非現実的だったため撤去し、チートは
//   完全外部の cheat_tester（メモリ走査＋WriteProcessMemory）に置き換えました。
//   typeA_wallhack_explained.md 参照。
//
// 外部チートDLL（game_cheat.dll）を必要時に読み込み、毎フレームの Tick を委譲する
// ローダー（PROMPT-H-01 / ステージH）。
//
// SECURITY NOTE: これは自作 GameEngine.exe の検証用。`--cheat` 指定時のみ Load() を
// 呼び、同ディレクトリに DLL が在る場合だけチートが有効になる。DLL が無ければ
// IsLoaded()==false となり Tick() は「何もしない（チート無し）」指示を返す。
class CheatModule {
public:
    ~CheatModule();

    // game_cheat.dll を作業ディレクトリから読み込む。成功で true。
    // 見つからない／ABI 不一致なら false（メッセージはログに出す）。
    bool Load(const char* dllName = "game_cheat.dll");

    bool IsLoaded() const { return m_tick != nullptr; }

    // in（秘匿情報）を DLL に渡し、out（透視指示）を得る。
    // 未ロード時は out を全 OFF にする（チートは一切効かない）。
    void Tick(const cheat::HostState& in, cheat::CheatCommand& out);

    void Unload();

private:
    // windows.h をヘッダに持ち込まないため HMODULE は void* で保持する。
    void* m_handle  = nullptr;
    int  (*m_version)()                                              = nullptr;
    void (*m_tick)(const cheat::HostState*, cheat::CheatCommand*)    = nullptr;
};
