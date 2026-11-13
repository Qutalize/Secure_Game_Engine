#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "engine/core/Gamepad.h"

// キーボード／マウス／ゲームパッド入力の状態を保持するクラス。
// Window から Win32 メッセージを受け取って状態を更新し、毎フレーム
// ゲームパッド（DualSense/DS4）をポーリングする。
// FPS 視点用に「カーソルをウィンドウ中央へ固定するモード」を持つ。
class Input {
public:
    // ---- Window 側から呼ばれるメッセージハンドラ ----
    void OnKeyDown(WPARAM vk);
    void OnKeyUp(WPARAM vk);
    void OnMouseMove(int x, int y);
    // Raw Input（WM_INPUT）からの相対移動量。カーソル位置に依存しないため
    // DPI スケーリングやカーソル中央固定のズレによる視点ドリフトが起きない。
    void OnRawMouse(int dx, int dy);
    void OnFocusLost();

    // ---- ゲームループから呼ぶ ----
    // 毎フレーム先頭で呼ぶ。カーソルロック中は中央へ戻してデルタを確定する。
    void BeginFrame(HWND hwnd);

    // ---- 照会 ----
    bool IsKeyDown(int vk) const;        // 例: IsKeyDown('W'), IsKeyDown(VK_SPACE)
    void GetMouseDelta(int& dx, int& dy) const { dx = m_mouseDeltaX; dy = m_mouseDeltaY; }

    // FPS 視点モードの ON/OFF。ON にするとカーソルを隠して中央固定する。
    void SetCursorLock(bool locked, HWND hwnd);
    bool IsCursorLocked() const { return m_cursorLocked; }

    // ---- ゲームパッド（DualSense/DS4）----
    bool IsPadConnected() const { return m_pad.IsConnected(); }
    // 左スティック（移動）。x=右+ / y=前+。デッドゾーン適用済み。
    void GetPadMove(float& x, float& y) const { x = m_pad.State().lx; y = m_pad.State().ly; }
    // 右スティック（視点）。x=右+ / y=上+。
    void GetPadLook(float& x, float& y) const { x = m_pad.State().rx; y = m_pad.State().ry; }
    bool IsPadDown(GamepadButton b) const { return m_pad.IsDown(b); }

private:
    Gamepad m_pad;   // DualSense/DS4（未接続でも無害。BeginFrame でポーリング）

    bool m_keys[256] = {};
    // Preserve a down/up pair that arrives between two updates (quick taps/clicks).
    bool m_pressPending[256] = {};
    bool m_pressedThisFrame[256] = {};

    int  m_lastMouseX = 0;
    int  m_lastMouseY = 0;
    bool m_haveMouse  = false;

    int  m_mouseDeltaX = 0;
    int  m_mouseDeltaY = 0;

    // Raw Input の相対移動を次の BeginFrame までためる（ロック中の視点に使う）。
    int  m_rawAccumX = 0;
    int  m_rawAccumY = 0;

    bool m_cursorLocked = false;
};
