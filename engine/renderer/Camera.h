#pragma once

#include <DirectXMath.h>

// シンプルなカメラ。位置と yaw/pitch（ラジアン）からビュー行列を作る。
// 移動ロジックそのものはゲーム側（Player）が担当し、ここは行列の供給に徹する。
// （PROMPT-04 の position/target/up 版から、FPS で扱いやすい yaw/pitch 版へ改良）
class Camera {
public:
    void SetPosition(const DirectX::XMFLOAT3& pos) { m_position = pos; }
    void SetRotation(float yaw, float pitch)       { m_yaw = yaw; m_pitch = pitch; }

    const DirectX::XMFLOAT3& GetPosition() const { return m_position; }
    float GetYaw()   const { return m_yaw; }
    float GetPitch() const { return m_pitch; }

    // yaw/pitch から前方向ベクトルを計算する（左手座標系）
    DirectX::XMVECTOR GetForward() const;

    DirectX::XMMATRIX GetViewMatrix() const;
    DirectX::XMMATRIX GetProjMatrix(float fovRadians, float aspect,
                                    float nearZ, float farZ) const;

private:
    DirectX::XMFLOAT3 m_position{ 0.0f, 0.0f, -3.0f };
    float m_yaw   = 0.0f; // Y 軸まわり（左右）
    float m_pitch = 0.0f; // X 軸まわり（上下）
};
