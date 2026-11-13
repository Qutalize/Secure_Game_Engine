#include "game/Player.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {
constexpr float kMoveSpeed    = 5.0f;     // m/s
constexpr float kSensitivity  = 0.0025f;  // rad / pixel（マウス）
constexpr float kPadLookSpeed = 2.6f;     // rad/s（右スティック視点）
constexpr float kPitchLimit   = 1.55f;    // 約 89 度
// プレイヤーの当たり判定半サイズ（幅 0.8m、身長 1.8m 相当）
const XMFLOAT3  kHalfExtent{ 0.4f, 0.9f, 0.4f };
}

void Player::Update(float dt, const Input& input, const std::vector<AABB>& obstacles) {
    // --- 視点回転 ---
    // マウス視点はカーソルロック中のみ有効。本プロジェクトは操作をコントローラー主体にし、
    // カーソルを解放している（別窓の cheat_tester を操作するため）ので、通常はマウスで視点が
    // 動かない＝コントローラー右スティックで視点操作する。
    if (input.IsCursorLocked()) {
        int dx = 0, dy = 0;
        input.GetMouseDelta(dx, dy);
        yaw   += static_cast<float>(dx) * kSensitivity;
        pitch += static_cast<float>(dy) * kSensitivity;
    }
    // 右スティック（-1..1）で視点。y は「上が正」なので pitch は符号反転して下向き加算に合わせる。
    float lookX = 0.0f, lookY = 0.0f;
    input.GetPadLook(lookX, lookY);
    yaw   += lookX * kPadLookSpeed * dt;
    pitch += -lookY * kPadLookSpeed * dt;
    pitch = std::clamp(pitch, -kPitchLimit, kPitchLimit);

    // --- 移動方向（XZ 平面、yaw から算出）---
    const float fwdX = sinf(yaw), fwdZ = cosf(yaw);
    const float rgtX = cosf(yaw), rgtZ = -sinf(yaw);

    float moveX = 0.0f, moveZ = 0.0f;
    if (input.IsKeyDown('W')) { moveX += fwdX; moveZ += fwdZ; }
    if (input.IsKeyDown('S')) { moveX -= fwdX; moveZ -= fwdZ; }
    if (input.IsKeyDown('D')) { moveX += rgtX; moveZ += rgtZ; }
    if (input.IsKeyDown('A')) { moveX -= rgtX; moveZ -= rgtZ; }
    // 左スティック（-1..1）で移動（WASD と加算）。y=前+ / x=右+。
    float padX = 0.0f, padY = 0.0f;
    input.GetPadMove(padX, padY);
    moveX += fwdX * padY + rgtX * padX;
    moveZ += fwdZ * padY + rgtZ * padX;

    // 斜め移動が速くならないよう正規化
    const float len = sqrtf(moveX * moveX + moveZ * moveZ);
    if (len > 1e-4f) {
        moveX = moveX / len * kMoveSpeed * dt;
        moveZ = moveZ / len * kMoveSpeed * dt;
    } else {
        moveX = moveZ = 0.0f;
    }

    // --- 衝突解決（当たり判定の中心は足元 + 半身長）---
    XMFLOAT3 center{ position.x, position.y + kHalfExtent.y, position.z };
    XMFLOAT3 velocity{ moveX, 0.0f, moveZ };
    XMFLOAT3 resolved = ResolveAABBCollision(center, kHalfExtent, velocity, obstacles);

    position.x = resolved.x;
    position.z = resolved.z;
    // position.y（足元）は床固定なので変更しない
}
