#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// PS5 DualSense / PS4 DualShock4 コントローラーを Windows の生 HID で読むクラス。
// 外部ツール（DS4Windows / Steam Input）不要でネイティブに動く。USB / Bluetooth 両対応。
//
// 設計メモ:
//  - SetupAPI で HID デバイスを列挙し、VID/PID で Sony パッドを見つけて開く。
//  - 入力レポートは overlapped ReadFile で「非ブロッキング」に読む（ゲームループを止めない）。
//  - レポート書式は機種（DualSense/DS4）× 接続（USB/BT）で異なるため、種別ごとにバイト位置を切替。
//  - 実機テストができないため、検出したデバイスとレポート ID/長さをログに出す（不一致時の調査用）。
//
// ※ hidsdi.h 等の実装依存ヘッダは .cpp 側にのみ置く（このヘッダは windows.h だけに依存）。
enum class GamepadButton {
    Cross = 0, Circle, Square, Triangle,   // ✕ 〇 □ △
    L1, R1, L2, R2,
    Create, Options, L3, R3, PS, Touchpad,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Count
};

struct GamepadState {
    // スティックは -1..1（デッドゾーン適用済み）。ly/ry は「上が正」に正規化。
    float lx = 0.0f, ly = 0.0f;
    float rx = 0.0f, ry = 0.0f;
    float l2 = 0.0f, r2 = 0.0f;            // アナログトリガ 0..1
    bool  buttons[static_cast<int>(GamepadButton::Count)] = {};
};

class Gamepad {
public:
    Gamepad();
    ~Gamepad();

    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    // Sony パッドを探して開く。見つかれば true。
    bool Connect();

    // 毎フレーム呼ぶ。溜まった入力レポートを非ブロッキングで消費し状態を更新する。
    // 未接続なら一定間隔で再接続を試みる（ホットプラグ対応）。
    void Poll();

    void Disconnect();
    bool IsConnected() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

    const GamepadState& State() const { return m_state; }
    bool IsDown(GamepadButton b) const;

private:
    bool OpenDevice();
    void Parse(const unsigned char* d, size_t len);

    HANDLE     m_handle = nullptr;
    OVERLAPPED m_ov{};
    bool       m_readPending = false;
    unsigned char m_buf[256] = {};
    size_t     m_inputLen = 64;

    // 0=none / 1=DualSense USB / 2=DualSense BT / 3=DS4 USB / 4=DS4 BT
    int m_kind = 0;
    int m_reconnectTimer = 0;

    GamepadState m_state;
};
