#pragma once

#include <cstdint>
#include <vector>
#include <DirectXMath.h>

#include "engine/core/Input.h"
#include "engine/math/Collision.h"

// プレイヤー。ShuffleAllocator::Handle<Player> で確保するため、
// 自明にコピー可能（trivially copyable）な POD に保つ。
// （仮想関数や std::vector などの非自明メンバを持たせないこと。
//   ShuffleSlots() がスロットを memcpy で移送するため。）
struct Player {
    DirectX::XMFLOAT3 position{ 0.0f, 0.0f, -6.0f }; // 足元の位置（y=0 が床）
    float    yaw    = 0.0f;
    float    pitch  = 0.0f;
    float    health = 100.0f;  // メモリスキャナのターゲットになる値
    uint32_t playerId = 1;

    // PROMPT-08 の InputState は engine の Input クラスがその役割を担う。
    void Update(float dt, const Input& input, const std::vector<AABB>& obstacles);

    // カメラの目線位置（足元 + 身長 1.7m）
    DirectX::XMFLOAT3 EyePosition() const {
        return { position.x, position.y + 1.7f, position.z };
    }
};
