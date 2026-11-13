#include "engine/core/Gamepad.h"

#include <cstdlib>
#include <cmath>

#include <setupapi.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include "engine/core/Logger.h"

// SetupAPI / HID のライブラリ（CMake でもリンクするが保険で pragma も置く）。
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace {

constexpr unsigned short kSonyVID       = 0x054C;
constexpr unsigned short kDualSensePID  = 0x0CE6;
constexpr unsigned short kDualSenseEdge = 0x0DF2;
constexpr unsigned short kDS4v1PID      = 0x05C4;
constexpr unsigned short kDS4v2PID      = 0x09CC;

constexpr float kDeadzone = 0.16f; // 少し広め（スティックのドリフトで視点が勝手に動くのを防ぐ）

// 生の 0..255 スティック値を -1..1 へ（128 中心）。
float Axis(unsigned char v) {
    float f = (static_cast<float>(v) - 128.0f) / 127.0f;
    if (f < -1.0f) f = -1.0f;
    if (f >  1.0f) f =  1.0f;
    return f;
}

// 半径デッドゾーンを 2 軸に適用（軸ごとではなく円形にして自然な操作感に）。
void ApplyDeadzone(float& x, float& y) {
    const float mag = sqrtf(x * x + y * y);
    if (mag < kDeadzone) { x = 0.0f; y = 0.0f; return; }
    // デッドゾーンの外側を 0..1 に引き伸ばす
    const float scaled = (mag - kDeadzone) / (1.0f - kDeadzone) / mag;
    x *= scaled;
    y *= scaled;
    if (x < -1.0f) x = -1.0f; if (x > 1.0f) x = 1.0f;
    if (y < -1.0f) y = -1.0f; if (y > 1.0f) y = 1.0f;
}

} // namespace

Gamepad::Gamepad() {
    m_ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); // manual reset
    Connect(); // 起動時に既に挿さっていれば即接続（未接続でも Poll が再試行する）
}

Gamepad::~Gamepad() {
    Disconnect();
    if (m_ov.hEvent) { CloseHandle(m_ov.hEvent); m_ov.hEvent = nullptr; }
}

bool Gamepad::Connect() {
    return OpenDevice();
}

bool Gamepad::OpenDevice() {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO info = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return false;

    bool opened = false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(info, nullptr, &hidGuid, i, &ifd) && !opened;
         ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(malloc(needed));
        if (detail == nullptr) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, needed, nullptr, nullptr)) {
            // 読み書き（BT の feature 要求のため）で開く。ダメなら読み取りのみ。
            HANDLE h = CreateFileW(detail->DevicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                h = CreateFileW(detail->DevicePath, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            }
            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attr{};
                attr.Size = sizeof(attr);
                bool isSony = false, isDualSense = false;
                if (HidD_GetAttributes(h, &attr) && attr.VendorID == kSonyVID) {
                    isDualSense = (attr.ProductID == kDualSensePID ||
                                   attr.ProductID == kDualSenseEdge);
                    isSony = isDualSense ||
                             (attr.ProductID == kDS4v1PID || attr.ProductID == kDS4v2PID);
                }

                if (isSony) {
                    // 入力レポート長で USB / BT を判定する。
                    PHIDP_PREPARSED_DATA pp = nullptr;
                    USHORT inLen = 64;
                    if (HidD_GetPreparsedData(h, &pp)) {
                        HIDP_CAPS caps{};
                        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                            inLen = caps.InputReportByteLength;
                        }
                        HidD_FreePreparsedData(pp);
                    }
                    const bool bt = (inLen >= 78);
                    if (isDualSense) m_kind = bt ? 2 : 1;
                    else             m_kind = bt ? 4 : 3;
                    m_inputLen = (inLen > 0 && inLen <= sizeof(m_buf)) ? inLen : 64;

                    // BT のときはフルレポートに切り替えるため feature を一度要求する。
                    if (bt) {
                        unsigned char f[64] = {};
                        f[0] = isDualSense ? 0x05 : 0x02; // DualSense=0x05 / DS4=0x02
                        HidD_GetFeature(h, f, sizeof(f));
                    }

                    m_handle = h;
                    m_readPending = false;
                    opened = true;
                    LOG_INFO("Gamepad 接続: %s (%s) / VID=0x%04X PID=0x%04X / 入力レポート %zu バイト",
                             isDualSense ? "DualSense" : "DualShock4",
                             bt ? "Bluetooth" : "USB",
                             attr.VendorID, attr.ProductID, m_inputLen);
                } else {
                    CloseHandle(h);
                }
            }
        }
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(info);
    return opened;
}

void Gamepad::Poll() {
    if (!IsConnected()) {
        // 未接続時は約 2 秒ごとに再接続を試す（60fps 前提の簡易カウンタ）。
        if (++m_reconnectTimer >= 120) {
            m_reconnectTimer = 0;
            Connect();
        }
        return;
    }

    // 溜まったレポートを最大 16 件ドレインして最新状態にする。
    for (int i = 0; i < 16; ++i) {
        if (!m_readPending) {
            ResetEvent(m_ov.hEvent);
            DWORD read = 0;
            if (ReadFile(m_handle, m_buf, static_cast<DWORD>(m_inputLen), &read, &m_ov)) {
                Parse(m_buf, read);
                continue;
            }
            const DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                m_readPending = true;
            } else {
                Disconnect();
                return;
            }
        }
        // 保留中の読み取りが完了しているか確認（待たない）。
        DWORD read = 0;
        if (GetOverlappedResult(m_handle, &m_ov, &read, FALSE)) {
            m_readPending = false;
            Parse(m_buf, read);
            continue;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_IO_INCOMPLETE) break; // まだ来ていない
        Disconnect();
        return;
    }
}

void Gamepad::Parse(const unsigned char* d, size_t len) {
    // 種別ごとのバイト位置。off = 左スティック X の位置。
    int off, l2i, r2i, bFace, bMain, bMisc;
    unsigned char reportId;
    if (m_kind == 1)      { reportId = 0x01; off = 1; l2i = 5; r2i = 6; bFace = 8;  bMain = 9;  bMisc = 10; }
    else if (m_kind == 2) { reportId = 0x31; off = 2; l2i = 6; r2i = 7; bFace = 9;  bMain = 10; bMisc = 11; }
    else if (m_kind == 3) { reportId = 0x01; off = 1; l2i = 8; r2i = 9; bFace = 5;  bMain = 6;  bMisc = 7;  }
    else if (m_kind == 4) { reportId = 0x11; off = 3; l2i = 10; r2i = 11; bFace = 7; bMain = 8;  bMisc = 9;  }
    else return;

    if (len < static_cast<size_t>(bMisc + 1)) return;
    if (d[0] != reportId) return; // 期待と違うレポート（BT の簡易レポート等）は無視

    GamepadState s;
    s.lx = Axis(d[off + 0]);
    s.ly = -Axis(d[off + 1]); // 画面上向きを正にする
    s.rx = Axis(d[off + 2]);
    s.ry = -Axis(d[off + 3]);
    ApplyDeadzone(s.lx, s.ly);
    ApplyDeadzone(s.rx, s.ry);
    s.l2 = static_cast<float>(d[l2i]) / 255.0f;
    s.r2 = static_cast<float>(d[r2i]) / 255.0f;

    const unsigned char face = d[bFace];
    const unsigned char mainByte = d[bMain];
    const unsigned char misc = d[bMisc];

    auto set = [&](GamepadButton b, bool v) { s.buttons[static_cast<int>(b)] = v; };
    set(GamepadButton::Square,   (face & 0x10) != 0);
    set(GamepadButton::Cross,    (face & 0x20) != 0);
    set(GamepadButton::Circle,   (face & 0x40) != 0);
    set(GamepadButton::Triangle, (face & 0x80) != 0);
    set(GamepadButton::L1,       (mainByte & 0x01) != 0);
    set(GamepadButton::R1,       (mainByte & 0x02) != 0);
    set(GamepadButton::L2,       (mainByte & 0x04) != 0 || s.l2 > 0.5f);
    set(GamepadButton::R2,       (mainByte & 0x08) != 0 || s.r2 > 0.5f);
    set(GamepadButton::Create,   (mainByte & 0x10) != 0);
    set(GamepadButton::Options,  (mainByte & 0x20) != 0);
    set(GamepadButton::L3,       (mainByte & 0x40) != 0);
    set(GamepadButton::R3,       (mainByte & 0x80) != 0);
    set(GamepadButton::PS,       (misc & 0x01) != 0);
    set(GamepadButton::Touchpad, (misc & 0x02) != 0);

    // D-pad は face 下位ニブルのハットスイッチ（0=上, 2=右, 4=下, 6=左, 8=中立）。
    const unsigned char hat = face & 0x0F;
    const bool up    = (hat == 7 || hat == 0 || hat == 1);
    const bool right = (hat == 1 || hat == 2 || hat == 3);
    const bool down  = (hat == 3 || hat == 4 || hat == 5);
    const bool left  = (hat == 5 || hat == 6 || hat == 7);
    set(GamepadButton::DpadUp, up);
    set(GamepadButton::DpadRight, right);
    set(GamepadButton::DpadDown, down);
    set(GamepadButton::DpadLeft, left);

    m_state = s;
}

bool Gamepad::IsDown(GamepadButton b) const {
    const int i = static_cast<int>(b);
    if (i < 0 || i >= static_cast<int>(GamepadButton::Count)) return false;
    return m_state.buttons[i];
}

void Gamepad::Disconnect() {
    if (m_readPending && m_handle) {
        CancelIo(m_handle);
        m_readPending = false;
    }
    if (m_handle && m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
    }
    m_handle = nullptr;
    m_kind = 0;
    m_state = GamepadState{};
}
