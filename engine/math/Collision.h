#pragma once

#include <vector>
#include <DirectXMath.h>

// 軸平行境界ボックス（Axis-Aligned Bounding Box）
struct AABB {
    DirectX::XMFLOAT3 min;
    DirectX::XMFLOAT3 max;
};

// 点が AABB 内部にあるか
bool PointInAABB(DirectX::XMFLOAT3 point, const AABB& box);

// 2 つの AABB が交差しているか
bool AABBIntersect(const AABB& a, const AABB& b);

// レイ（origin から dir 方向。dir は正規化済みを推奨）と AABB の交差判定。
// 交差すれば true を返し、origin からの距離（dir の倍率）を outT に格納する。
// origin が箱の内部にある場合は outT=0。FPS の射撃（ヒットスキャン）に使う。
bool RayIntersectAABB(const DirectX::XMFLOAT3& origin,
                      const DirectX::XMFLOAT3& dir,
                      const AABB& box, float& outT);

// キャラクター（中心 + 半サイズ）が velocity だけ移動するとき、
// 障害物との衝突を解決した最終位置を返す。軸ごとに分離して解決し、
// 壁にめり込まず「滑る」挙動にする。
DirectX::XMFLOAT3 ResolveAABBCollision(
    DirectX::XMFLOAT3 position,
    DirectX::XMFLOAT3 halfExtent,
    DirectX::XMFLOAT3 velocity,
    const std::vector<AABB>& obstacles);
