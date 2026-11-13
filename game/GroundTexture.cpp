#include "game/GroundTexture.h"

#include <cmath>

// ===========================================================================
// 地面テクスチャの手続き生成（PROMPT-I-02）。
//  - 決定論的な値ノイズ（ハッシュ）で自然なムラを作る
//  - タイリング前提（WRAP）。継ぎ目を完全には消せないが単調さは大きく軽減する
// ===========================================================================
namespace groundtex {
namespace {

struct Color { uint8_t r, g, b, a; };

void Set(std::vector<uint8_t>& px, int w, int x, int y, Color c) {
    uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
}

uint8_t U8(float f) {
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return static_cast<uint8_t>(f * 255.0f + 0.5f);
}

// 2D 整数ハッシュ（0..1）。タイル境界で連続になるよう座標は w/h で剰余を取る。
float Hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u +
                 static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return static_cast<float>(h & 0xFFFF) / 65535.0f;
}

// タイル可能な値ノイズ（格子点をラップして補間）。
float Noise(float u, float v, int period) {
    const float x = u * period;
    const float y = v * period;
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float fx = x - xi;
    const float fy = y - yi;
    auto h = [&](int gx, int gy) {
        return Hash(((gx % period) + period) % period, ((gy % period) + period) % period);
    };
    const float a = h(xi, yi),     b = h(xi + 1, yi);
    const float c = h(xi, yi + 1), d = h(xi + 1, yi + 1);
    const float sx = fx * fx * (3.0f - 2.0f * fx);
    const float sy = fy * fy * (3.0f - 2.0f * fy);
    return (a + (b - a) * sx) * (1.0f - sy) + (c + (d - c) * sx) * sy;
}

// 複数周波数を重ねた fBm（0..1 目安）。
float Fbm(float u, float v) {
    float f = 0.0f, amp = 0.5f;
    int period = 4;
    for (int o = 0; o < 4; ++o) {
        f += Noise(u, v, period) * amp;
        amp *= 0.5f;
        period *= 2;
    }
    return f;
}

} // namespace

void RenderGrass(int width, int height, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            const float n = Fbm(u, v);
            // 緑を基調に、ノイズで明暗と黄緑〜濃緑の変化をつける。
            const float g = 0.32f + 0.22f * n;
            const float r = 0.12f + 0.16f * n;
            const float b = 0.08f + 0.10f * n;
            // 草の筋（高周波の縦ノイズ）で少し明るい斑を足す。
            const float blade = Noise(u * 1.0f, v * 1.0f, 64);
            const float hi = (blade > 0.78f) ? 0.12f : 0.0f;
            Set(out, width, x, y, Color{ U8(r + hi), U8(g + hi), U8(b + hi * 0.5f), 255 });
        }
    }
}

void RenderConcrete(int width, int height, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    const int tile = width / 2; // 目地（タイル 2x2）
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            const float n = Fbm(u, v);
            float g = 0.42f + 0.16f * n;          // 灰色ベース＋ムラ
            // 斑点（暗い染み）
            const float spot = Noise(u, v, 48);
            if (spot > 0.85f) g -= 0.12f;
            // 目地（タイルの境界を暗い線に）
            const int mx = x % tile, my = y % tile;
            if (mx < 2 || my < 2) g -= 0.18f;
            Set(out, width, x, y, Color{ U8(g * 0.98f), U8(g), U8(g * 1.02f), 255 });
        }
    }
}

void RenderBrick(int width, int height, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    const int brickW = width / 4;   // 横 4 枚
    const int brickH = height / 8;  // 縦 8 段
    const int mortar = 3;           // 目地の太さ（px）
    for (int y = 0; y < height; ++y) {
        const int row  = y / brickH;
        const int xoff = (row % 2) * (brickW / 2); // 一段おきに半個ずらす（走り目地）
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            const float n = Fbm(u, v);

            const int bx = (x + xoff) % brickW;
            const int by = y % brickH;
            const bool isMortar = (bx < mortar) || (by < mortar);

            Color c;
            if (isMortar) {
                const float g = 0.60f + 0.08f * n; // モルタル（明るい灰）
                c = Color{ U8(g), U8(g), U8(g * 0.98f), 255 };
            } else {
                // レンガごとに色を少し変える（ハッシュで決定論的に）。
                const int bcol = (x + xoff) / brickW;
                const float tint = 0.85f + 0.30f * Hash(bcol, row);
                const float r = (0.52f * tint) * (0.85f + 0.30f * n);
                const float gg = (0.22f * tint) * (0.85f + 0.30f * n);
                const float b = (0.16f * tint) * (0.85f + 0.30f * n);
                c = Color{ U8(r), U8(gg), U8(b), 255 };
            }
            Set(out, width, x, y, c);
        }
    }
}

void RenderWoodCrate(int width, int height, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    const int plankW = width / 6;         // 縦板 6 枚
    const int border = width / 14;        // 外枠の太さ
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            const float n = Fbm(u, v);

            // 木の基調色＋ムラ。
            float r = 0.48f * (0.80f + 0.40f * n);
            float g = 0.32f * (0.80f + 0.40f * n);
            float b = 0.16f * (0.80f + 0.40f * n);

            // 木目（横方向の細い縞）。
            const float grain = Noise(u * 2.0f, v * 10.0f, 32);
            const float gm = 0.90f + 0.15f * grain;
            r *= gm; g *= gm; b *= gm;

            // 縦板の隙間（暗い線）。
            const int px = x % plankW;
            if (px < 2) { r *= 0.45f; g *= 0.45f; b *= 0.45f; }

            // 外枠（少し明るい縁取り）＝箱らしさを出す。
            if (x < border || x >= width - border || y < border || y >= height - border) {
                r = 0.55f * (0.9f + 0.2f * n);
                g = 0.38f * (0.9f + 0.2f * n);
                b = 0.20f * (0.9f + 0.2f * n);
            }
            Set(out, width, x, y, Color{ U8(r), U8(g), U8(b), 255 });
        }
    }
}

} // namespace groundtex
