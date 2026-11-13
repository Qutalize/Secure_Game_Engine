#include "engine/renderer/Camera.h"

using namespace DirectX;

XMVECTOR Camera::GetForward() const {
    // pitch を [-89°, +89°] 相当にクランプするのは呼び出し側（Player）に任せる。
    const float cosPitch = cosf(m_pitch);
    XMVECTOR forward = XMVectorSet(
        sinf(m_yaw) * cosPitch,
        sinf(m_pitch),
        cosf(m_yaw) * cosPitch,
        0.0f);
    return XMVector3Normalize(forward);
}

XMMATRIX Camera::GetViewMatrix() const {
    XMVECTOR eye     = XMLoadFloat3(&m_position);
    XMVECTOR forward = GetForward();
    XMVECTOR up      = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    // 左手系の LookTo（方向ベクトル指定）
    return XMMatrixLookToLH(eye, forward, up);
}

XMMATRIX Camera::GetProjMatrix(float fovRadians, float aspect,
                               float nearZ, float farZ) const {
    return XMMatrixPerspectiveFovLH(fovRadians, aspect, nearZ, farZ);
}
