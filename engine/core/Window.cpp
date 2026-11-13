#include "engine/core/Window.h"

#include "engine/core/Logger.h"

namespace {
    constexpr wchar_t kWindowClassName[] = L"AntiCheatEngineWindowClass";
}

bool Window::Initialize(HINSTANCE hInstance, int width, int height, const wchar_t* title) {
    m_width  = width;
    m_height = height;

    // DPI 対応（高DPIディスプレイでクライアント/スクリーン座標を 1:1 にする）。
    // これをしないと Win11 のスケーリング下でカーソル拘束や座標計算がズレる。
    SetProcessDPIAware();

    // ウィンドウクラスの登録
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &Window::WindowProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("RegisterClassExW failed (0x%08X)", GetLastError());
        return false;
    }

    // クライアント領域が width x height になるようウィンドウ全体サイズを調整
    RECT rc{ 0, 0, width, height };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&rc, style, FALSE);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    // 画面中央に配置（大きめウィンドウが画面外/見切れないように）。
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW > winW) ? (screenW - winW) / 2 : 0;
    int posY = (screenH > winH) ? (screenH - winH) / 2 : 0;

    m_hwnd = CreateWindowExW(
        0, kWindowClassName, title, style,
        posX, posY, winW, winH,
        nullptr, nullptr, hInstance, this); // 末尾の this を WM_NCCREATE で受け取る

    if (!m_hwnd) {
        LOG_ERROR("CreateWindowExW failed (0x%08X)", GetLastError());
        return false;
    }

    // Raw Input（相対マウス）を登録。FPS 視点をカーソル位置に依存させないため。
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // Generic Desktop
    rid.usUsage     = 0x02; // Mouse
    rid.dwFlags     = 0;    // フォーカス時のみ受信（ゲーム向き）
    rid.hwndTarget  = m_hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        LOG_WARN("RegisterRawInputDevices failed (0x%08X)。マウス視点は従来動作にフォールバック。",
                 GetLastError());
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    // ★重要: ゲームウィンドウを前面＋キーボードフォーカスにする。
    // コンソール起動だとフォーカスがコンソール側に残り、キーボード（ESC など）が
    // ゲームに届かない（コントローラーは HID 直読みで動くので気付きにくい）。
    SetForegroundWindow(m_hwnd);
    SetFocus(m_hwnd);

    LOG_INFO("Window created: %dx%d", width, height);
    return true;
}

bool Window::ProcessMessages() {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // CreateWindowExW の lpParam で渡した this を GWLP_USERDATA に保存する。
    if (msg == WM_NCCREATE) {
        auto* cs   = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = reinterpret_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    }

    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            // ESC で終了（PROMPT-01 の完了条件）
            PostQuitMessage(0);
            return 0;
        }
        if (self) self->m_input.OnKeyDown(wParam);
        return 0;

    case WM_KEYUP:
        if (self) self->m_input.OnKeyUp(wParam);
        return 0;

    // マウスボタンは仮想キー(VK_LBUTTON/VK_RBUTTON)として Input に流す。
    // これで IsKeyDown(VK_LBUTTON) で射撃入力を取れる（FPS の射撃に使用）。
    case WM_LBUTTONDOWN:
        if (self) self->m_input.OnKeyDown(VK_LBUTTON);
        return 0;
    case WM_LBUTTONUP:
        if (self) self->m_input.OnKeyUp(VK_LBUTTON);
        return 0;
    case WM_RBUTTONDOWN:
        if (self) self->m_input.OnKeyDown(VK_RBUTTON);
        return 0;
    case WM_RBUTTONUP:
        if (self) self->m_input.OnKeyUp(VK_RBUTTON);
        return 0;

    case WM_MOUSEMOVE:
        if (self) {
            // lParam の下位/上位 16bit が x/y（符号付きとして扱う）
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            self->m_input.OnMouseMove(x, y);
        }
        return 0;

    case WM_INPUT:
        // Raw Input（相対マウス移動）。カーソル位置に依存しない視点入力。
        if (self) {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER));
            BYTE buffer[sizeof(RAWINPUT)];
            if (size > 0 && size <= sizeof(buffer) &&
                GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                                buffer, &size, sizeof(RAWINPUTHEADER)) == size) {
                auto* ri = reinterpret_cast<RAWINPUT*>(buffer);
                if (ri->header.dwType == RIM_TYPEMOUSE &&
                    (ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    self->m_input.OnRawMouse(ri->data.mouse.lLastX, ri->data.mouse.lLastY);
                }
            }
        }
        // WM_INPUT は既定処理も呼ぶこと（システム側のクリーンアップのため）。
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_KILLFOCUS:
        if (self) self->m_input.OnFocusLost();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
