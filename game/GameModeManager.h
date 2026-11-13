#pragma once

#include <memory>
#include <string>
#include <vector>

#include "game/IGameMode.h"

// 登録された複数の IGameMode を保持し、起動引数(--mode <name>)で
// 1つを選んで駆動するマネージャ（PROMPT-V3-01）。
// V3 の複数ゲームモード切替の中枢。
class GameModeManager {
public:
    // モードを登録する（所有権を受け取る）。名前は mode->GetName() を使う。
    void Register(std::unique_ptr<IGameMode> mode);

    // 名前でモードを選び、ctx で Init する。
    // 見つからない／初期化失敗なら false（メッセージはログに出力）。
    bool Select(const std::string& name, const GameContext& ctx);

    // 現在のモード（未選択なら nullptr）。
    IGameMode* Current() const { return m_current; }

    // 登録済みモード名を "fps, card, tag" の形で返す（エラーメッセージ用）。
    std::string ListNames() const;

private:
    std::vector<std::unique_ptr<IGameMode>> m_modes;   // 所有
    IGameMode*                              m_current = nullptr; // 借用
};
