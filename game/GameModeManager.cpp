#include "game/GameModeManager.h"

#include "engine/core/Logger.h"

void GameModeManager::Register(std::unique_ptr<IGameMode> mode) {
    m_modes.push_back(std::move(mode));
}

bool GameModeManager::Select(const std::string& name, const GameContext& ctx) {
    for (std::unique_ptr<IGameMode>& mode : m_modes) {
        if (name == mode->GetName()) {
            if (!mode->Init(ctx)) {
                LOG_ERROR("ゲームモード '%s' の初期化に失敗しました", name.c_str());
                return false;
            }
            m_current = mode.get();
            LOG_INFO("ゲームモード '%s' を開始します", name.c_str());
            return true;
        }
    }
    LOG_ERROR("不明なゲームモード '%s'。利用可能: %s", name.c_str(), ListNames().c_str());
    return false;
}

std::string GameModeManager::ListNames() const {
    std::string out;
    for (size_t i = 0; i < m_modes.size(); ++i) {
        if (i != 0) out += ", ";
        out += m_modes[i]->GetName();
    }
    return out;
}
