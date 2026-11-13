#include "engine/math/Collision.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

bool PointInAABB(XMFLOAT3 p, const AABB& box) {
    return p.x >= box.min.x && p.x <= box.max.x &&
           p.y >= box.min.y && p.y <= box.max.y &&
           p.z >= box.min.z && p.z <= box.max.z;
}

bool AABBIntersect(const AABB& a, const AABB& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x &&
           a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

bool RayIntersectAABB(const XMFLOAT3& origin, const XMFLOAT3& dir,
                      const AABB& box, float& outT) {
    // スラブ法: 各軸で「レイが箱の板(slab)に入る/出る」区間を求め、共通区間を取る。
    float tmin = 0.0f;            // 後方(負の距離)のヒットは無視するため 0 始まり
    float tmax = 3.4e38f;         // 実質無限大

    const float o[3]   = { origin.x, origin.y, origin.z };
    const float d[3]   = { dir.x,    dir.y,    dir.z    };
    const float lo[3]  = { box.min.x, box.min.y, box.min.z };
    const float hi[3]  = { box.max.x, box.max.y, box.max.z };

    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-8f) {
            // レイがこの軸にほぼ平行: 原点がスラブ外なら絶対に交差しない
            if (o[i] < lo[i] || o[i] > hi[i]) return false;
        } else {
            const float inv = 1.0f / d[i];
            float t1 = (lo[i] - o[i]) * inv;
            float t2 = (hi[i] - o[i]) * inv;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false; // 共通区間が無い＝交差なし
        }
    }
    outT = tmin;
    return true;
}

namespace {
// 中心 + 半サイズから AABB を作る
AABB MakeBox(const XMFLOAT3& center, const XMFLOAT3& half) {
    return AABB{
        { center.x - half.x, center.y - half.y, center.z - half.z },
        { center.x + half.x, center.y + half.y, center.z + half.z }
    };
}
}

XMFLOAT3 ResolveAABBCollision(XMFLOAT3 position, XMFLOAT3 halfExtent,
                              XMFLOAT3 velocity,
                              const std::vector<AABB>& obstacles) {
    // まず移動を適用し、その後「最小めり込み軸（MTV）」で押し出して重なりを解消する。
    //
    // 旧実装は軸ごとに「速度の符号で障害物の min/max 面へスナップ」していたが、
    // 外周壁のように片方の軸に極端に長い箱では、接触していない側の遠い面へ
    // 座標が飛んでしまい「場外に追い出される／動けなくなる」不具合があった。
    // ここでは接触方向（重なりが最小の軸）へ、めり込んだぶんだけ押し戻す。
    //
    // ※ 地上キャラ用に押し出しは水平（X/Z）のみ行う（呼び出し側は y を使わない）。
    //   Y は 3D 交差判定にのみ使う（高さが被っているときだけ衝突とみなす）。
    XMFLOAT3 pos = position;
    pos.x += velocity.x;
    pos.y += velocity.y;
    pos.z += velocity.z;

    // 角などで複数の障害物に同時に重なる場合に備え、数回反復して収束させる。
    for (int iter = 0; iter < 4; ++iter) {
        bool resolvedAny = false;
        for (const AABB& o : obstacles) {
            AABB me = MakeBox(pos, halfExtent);

            // 各軸の重なり幅（正なら重なっている）。0 以下＝接触のみ/離れている。
            const float ox = std::min(me.max.x, o.max.x) - std::max(me.min.x, o.min.x);
            const float oy = std::min(me.max.y, o.max.y) - std::max(me.min.y, o.min.y);
            const float oz = std::min(me.max.z, o.max.z) - std::max(me.min.z, o.min.z);
            if (ox <= 0.0f || oy <= 0.0f || oz <= 0.0f) continue; // 交差していない

            resolvedAny = true;

            // 水平方向で重なりが小さい軸へ、その量だけ押し戻す。
            const float meCx = (me.min.x + me.max.x) * 0.5f;
            const float oCx  = (o.min.x  + o.max.x)  * 0.5f;
            const float meCz = (me.min.z + me.max.z) * 0.5f;
            const float oCz  = (o.min.z  + o.max.z)  * 0.5f;
            if (ox <= oz) {
                pos.x += (meCx < oCx) ? -ox : ox;
            } else {
                pos.z += (meCz < oCz) ? -oz : oz;
            }
        }
        if (!resolvedAny) break;
    }

    return pos;
}
