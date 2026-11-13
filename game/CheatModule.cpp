#include "game/CheatModule.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "engine/core/Logger.h"

CheatModule::~CheatModule() {
    Unload();
}

bool CheatModule::Load(const char* dllName) {
    HMODULE h = LoadLibraryA(dllName);
    if (h == nullptr) {
        // ここが「チートファイルが無い＝チート不可」の分岐。
        LOG_WARN("cheat module '%s' not found（チートDLLが無いためチートは無効）。", dllName);
        return false;
    }

    // 関数解決（extern "C" なので名前修飾なし）。FARPROC → 関数ポインタは reinterpret_cast。
    auto ver = reinterpret_cast<int (*)()>(GetProcAddress(h, "CheatApiVersion"));
    auto tick = reinterpret_cast<void (*)(const cheat::HostState*, cheat::CheatCommand*)>(
        GetProcAddress(h, "CheatTick"));
    if (ver == nullptr || tick == nullptr) {
        LOG_ERROR("cheat module のエクスポート関数が見つかりません（破損 DLL?）。");
        FreeLibrary(h);
        return false;
    }
    if (ver() != cheat::kApiVersion) {
        LOG_ERROR("cheat module の ABI 不一致（DLL=%d, 期待=%d）。再ビルドしてください。",
                  ver(), cheat::kApiVersion);
        FreeLibrary(h);
        return false;
    }

    m_handle  = h;
    m_version = ver;
    m_tick    = tick;
    LOG_WARN("cheat module loaded（game_cheat.dll, api v%d）＝チート有効。O キーで透視 ON/OFF。",
             ver());
    return true;
}

void CheatModule::Tick(const cheat::HostState& in, cheat::CheatCommand& out) {
    out = cheat::CheatCommand{}; // 既定は全 OFF（未ロード時はチート無し）
    if (m_tick != nullptr) m_tick(&in, &out);
}

void CheatModule::Unload() {
    if (m_handle != nullptr) {
        FreeLibrary(static_cast<HMODULE>(m_handle));
        m_handle = nullptr;
    }
    m_version = nullptr;
    m_tick    = nullptr;
}
