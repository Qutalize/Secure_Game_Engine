#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "engine/core/Input.h"

// Win32 ウィンドウのラッパー。
// ウィンドウの生成・メッセージポンプ・入力の集約を担当する。
class Window {
public:
    bool Initialize(HINSTANCE hInstance, int width, int height, const wchar_t* title);

    // メッセージを処理する。WM_QUIT を受信したら false を返す。
    bool ProcessMessages();

    HWND  GetHandle() const { return m_hwnd; }
    int   GetWidth()  const { return m_width; }
    int   GetHeight() const { return m_height; }

    Input&       GetInput()       { return m_input; }
    const Input& GetInput() const { return m_input; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND  m_hwnd   = nullptr;
    int   m_width  = 0;
    int   m_height = 0;
    Input m_input;
};
