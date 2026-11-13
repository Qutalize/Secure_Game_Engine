#include "engine/core/Input.h"

void Input::OnKeyDown(WPARAM vk) {
    if (vk < 256) { if(!m_keys[vk])m_pressPending[vk]=true; m_keys[vk] = true; }
}

void Input::OnKeyUp(WPARAM vk) {
    if (vk < 256) m_keys[vk] = false;
}

void Input::OnMouseMove(int x, int y) {
    // カーソルロック中は BeginFrame() 側でデルタを計算するため、
    // ここでの差分蓄積はロックしていない時のみ行う。
    if (m_cursorLocked) return;

    if (m_haveMouse) {
        m_mouseDeltaX += x - m_lastMouseX;
        m_mouseDeltaY += y - m_lastMouseY;
    }
    m_lastMouseX = x;
    m_lastMouseY = y;
    m_haveMouse  = true;
}

void Input::OnRawMouse(int dx, int dy) {
    // 相対移動を蓄積（BeginFrame でこのフレームぶんを確定する）。
    m_rawAccumX += dx;
    m_rawAccumY += dy;
}

void Input::OnFocusLost() {
    // フォーカスを失ったら押下状態をクリア（キーが押しっぱなし扱いになるのを防ぐ）
    for (bool& k : m_keys) k = false;
    for (bool& k : m_pressPending) k = false;
    for (bool& k : m_pressedThisFrame) k = false;
    // カーソルの拘束も解除しておく（他ウィンドウ操作の邪魔をしない）
    ClipCursor(nullptr);
}

void Input::BeginFrame(HWND hwnd) {
    for(int i=0;i<256;++i){m_pressedThisFrame[i]=m_pressPending[i];m_pressPending[i]=false;}
    // ゲームパッド（DualSense/DS4）を毎フレーム更新（未接続なら再接続を試行）。
    m_pad.Poll();

    if (m_cursorLocked && hwnd != nullptr) {
        // ロック中は Raw Input の相対移動をこのフレームの視点デルタとして確定する。
        // カーソル位置に依存しないので DPI スケーリングでもドリフトしない。
        m_mouseDeltaX = m_rawAccumX;
        m_mouseDeltaY = m_rawAccumY;
        m_rawAccumX = 0;
        m_rawAccumY = 0;

        // 隠しカーソルがウィンドウ外へ出て誤クリック／フォーカス喪失しないよう拘束する。
        RECT rc;
        GetClientRect(hwnd, &rc);
        POINT tl{ rc.left, rc.top };
        POINT br{ rc.right, rc.bottom };
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);
        RECT clip{ tl.x, tl.y, br.x, br.y };
        ClipCursor(&clip);
    } else {
        // 非ロック時は視点デルタなし。ためた Raw 移動は捨てる。
        m_mouseDeltaX = 0;
        m_mouseDeltaY = 0;
        m_rawAccumX = 0;
        m_rawAccumY = 0;
    }
}

bool Input::IsKeyDown(int vk) const {
    if (vk < 0 || vk >= 256) return false;
    return m_keys[vk] || m_pressedThisFrame[vk];
}

void Input::SetCursorLock(bool locked, HWND hwnd) {
    if (locked == m_cursorLocked) return;
    m_cursorLocked = locked;

    if (locked) {
        ShowCursor(FALSE);
        // 実際の拘束（ClipCursor）は BeginFrame で毎フレーム行う（ウィンドウ移動に追従）。
    } else {
        ShowCursor(TRUE);
        ClipCursor(nullptr); // 拘束解除
    }
}
