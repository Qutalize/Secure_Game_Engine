#pragma once

// シンプルなロガー。OutputDebugStringA（VS の出力ウィンドウ）と
// printf（コンソール）の両方へ出力する。

namespace engine {
    // 内部実装。直接は呼ばず下のマクロを使う。
    void LogMessage(const char* level, const char* file, int line, const char* fmt, ...);
}

#ifdef PROTECTED_DISTRIBUTION
#define ENGINE_LOG_FILE ""
#define ENGINE_LOG_LINE 0
#else
#define ENGINE_LOG_FILE __FILE__
#define ENGINE_LOG_LINE __LINE__
#endif
#define LOG_INFO(...)  ::engine::LogMessage("INFO", ENGINE_LOG_FILE, ENGINE_LOG_LINE, __VA_ARGS__)
#define LOG_WARN(...)  ::engine::LogMessage("WARN", ENGINE_LOG_FILE, ENGINE_LOG_LINE, __VA_ARGS__)
#define LOG_ERROR(...) ::engine::LogMessage("ERROR", ENGINE_LOG_FILE, ENGINE_LOG_LINE, __VA_ARGS__)
