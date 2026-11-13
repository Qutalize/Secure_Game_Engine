#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "engine/renderer/Renderer.h"   // Vertex

// ===========================================================================
// キャラクター用の簡易メッシュビルダー（視覚増分3.1：丸みのあるローポリ）。
//
// 「箱の積み重ね」に見えないよう、腕/脚/胴を「テーパーした多角柱（既定 8 角形＝八角柱）」で
// 作る。断面を 8 角形にするだけでも輪郭の角ばりが大きく減り、さらに側面の法線を
// 「軸から放射状（スムーズ法線）」にすることで、ピクセル補間で円柱のように滑らかに陰影が
// 付く（＝ローポリでもカクカクして見えない）。上面/底面の半径を変えると先細りになる。
//
// ヘッダオンリー（inline）＝新規 .cpp 不要。頂点色（Vertex）メッシュ用。
// ===========================================================================
namespace charmesh {

constexpr int kRoundSides = 8; // 断面の分割数（8=八角柱。増やすほど丸いが重い）

// 4 頂点の面を、法線を centroid から外向きに向けて追加する。
inline void AddQuadOut(std::vector<Vertex>& v, std::vector<uint32_t>& idx,
                       const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1,
                       const DirectX::XMFLOAT3& p2, const DirectX::XMFLOAT3& p3,
                       const DirectX::XMFLOAT3& centroid, const DirectX::XMFLOAT4& color) {
    using DirectX::XMFLOAT3;
    auto sub = [](const XMFLOAT3& a, const XMFLOAT3& b) {
        return XMFLOAT3{ a.x - b.x, a.y - b.y, a.z - b.z };
    };
    const XMFLOAT3 e1 = sub(p1, p0);
    const XMFLOAT3 e2 = sub(p3, p0);
    XMFLOAT3 n{ e1.y * e2.z - e1.z * e2.y,
                e1.z * e2.x - e1.x * e2.z,
                e1.x * e2.y - e1.y * e2.x };
    const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-6f) { n.x /= len; n.y /= len; n.z /= len; }
    // 面の中心が重心より外側になる向きへ法線を揃える。
    const XMFLOAT3 fc{ (p0.x + p1.x + p2.x + p3.x) * 0.25f,
                       (p0.y + p1.y + p2.y + p3.y) * 0.25f,
                       (p0.z + p1.z + p2.z + p3.z) * 0.25f };
    const XMFLOAT3 outv = sub(fc, centroid);
    if (n.x * outv.x + n.y * outv.y + n.z * outv.z < 0.0f) { n.x = -n.x; n.y = -n.y; n.z = -n.z; }

    const uint32_t base = static_cast<uint32_t>(v.size());
    v.push_back(Vertex{ p0, n, color });
    v.push_back(Vertex{ p1, n, color });
    v.push_back(Vertex{ p2, n, color });
    v.push_back(Vertex{ p3, n, color });
    idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
    idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
}

// テーパーした八角柱（丸みのある筒）を追加する。中心 (cx, cz)、底面 y0（半径 bx,bz）→
// 上面 y1（半径 tx,tz）。y0<y1。側面法線は軸から放射状（スムーズ）＝円柱状に滑らかに見える。
// bx==bz なら円柱、異なれば楕円柱（肩幅など）。上下で半径を変えると先細り。
inline void AddTaper(std::vector<Vertex>& v, std::vector<uint32_t>& idx,
                     float cx, float cz, float y0, float y1,
                     float bx, float bz, float tx, float tz, const DirectX::XMFLOAT4& color) {
    using DirectX::XMFLOAT3;
    const int   N   = kRoundSides;
    const float TAU = 6.28318530718f;

    auto pos = [&](float ang, float rx, float rz, float y) {
        return XMFLOAT3{ cx + rx * std::cos(ang), y, cz + rz * std::sin(ang) };
    };
    // 楕円の外向き法線（水平放射）。角度が連続なので隣接面で法線が滑らかにつながる。
    auto nrm = [&](float ang, float rx, float rz) {
        const float ex = (rx > 1e-4f) ? rx : 1e-4f;
        const float ez = (rz > 1e-4f) ? rz : 1e-4f;
        XMFLOAT3 n{ std::cos(ang) / ex, 0.0f, std::sin(ang) / ez };
        const float l = std::sqrt(n.x * n.x + n.z * n.z);
        if (l > 1e-6f) { n.x /= l; n.z /= l; }
        return n;
    };

    // 側面（N 枚の四角形。各頂点に放射状スムーズ法線）
    for (int i = 0; i < N; ++i) {
        const float a0 = TAU * i / N;
        const float a1 = TAU * (i + 1) / N;
        const XMFLOAT3 b0 = pos(a0, bx, bz, y0), b1 = pos(a1, bx, bz, y0);
        const XMFLOAT3 t0 = pos(a0, tx, tz, y1), t1 = pos(a1, tx, tz, y1);
        const XMFLOAT3 nb0 = nrm(a0, bx, bz), nb1 = nrm(a1, bx, bz);
        const XMFLOAT3 nt0 = nrm(a0, tx, tz), nt1 = nrm(a1, tx, tz);
        const uint32_t base = static_cast<uint32_t>(v.size());
        v.push_back(Vertex{ b0, nb0, color });
        v.push_back(Vertex{ b1, nb1, color });
        v.push_back(Vertex{ t1, nt1, color });
        v.push_back(Vertex{ t0, nt0, color });
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    }

    // 上下の蓋（三角ファン。法線は上下向き。多くは関節に隠れるが閉じておく）
    auto cap = [&](float y, float rx, float rz, const XMFLOAT3& capN, bool flip) {
        const uint32_t cIdx = static_cast<uint32_t>(v.size());
        v.push_back(Vertex{ XMFLOAT3{ cx, y, cz }, capN, color }); // 中心
        for (int i = 0; i < N; ++i)
            v.push_back(Vertex{ pos(TAU * i / N, rx, rz, y), capN, color });
        for (int i = 0; i < N; ++i) {
            const uint32_t a = cIdx + 1 + i;
            const uint32_t b = cIdx + 1 + ((i + 1) % N);
            if (flip) { idx.push_back(cIdx); idx.push_back(b); idx.push_back(a); }
            else      { idx.push_back(cIdx); idx.push_back(a); idx.push_back(b); }
        }
    };
    cap(y0, bx, bz, XMFLOAT3{ 0.0f, -1.0f, 0.0f }, false); // 底
    cap(y1, tx, tz, XMFLOAT3{ 0.0f,  1.0f, 0.0f }, true);  // 上
}

} // namespace charmesh
