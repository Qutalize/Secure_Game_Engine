#pragma once

#include <cassert>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "engine/core/Logger.h"

// CLAUDE.md 規定の HRESULT チェックマクロ。
// 失敗時はメッセージを出力して assert で停止する。
#define DX_CHECK(hr)                                                        \
    do {                                                                    \
        HRESULT _hr = (hr);                                                 \
        if (FAILED(_hr)) {                                                  \
            char _msg[256];                                                 \
            sprintf_s(_msg, "DX12 Error: HRESULT 0x%08X at %s:%d\n",        \
                      static_cast<unsigned>(_hr), ENGINE_LOG_FILE, ENGINE_LOG_LINE); \
            OutputDebugStringA(_msg);                                       \
            fputs(_msg, stderr);                                            \
            assert(false && "DX_CHECK failed");                            \
        }                                                                   \
    } while (0)
