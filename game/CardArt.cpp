#include "game/CardArt.h"

#include <cmath>
#include <cstring>

// ===========================================================================
// トランプ絵柄の手続き生成（PROMPT-G-01）
//   - 5x7 ビットマップフォントで数字・英字を描く
//   - スート記号は正規化座標での陰関数テストで塗る（♥♦♣♠）
// テクスチャファイル不要でカードに数字/スートを表示できる。
// ===========================================================================
namespace cardart {
namespace {

struct Color { uint8_t r, g, b, a; };

// 1 ピクセルのキャンバス（RGBA8, 行優先）。
struct Canvas {
    int w, h;
    std::vector<uint8_t>& px;

    void Set(int x, int y, Color c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
        p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
    }
    void Fill(Color c) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) Set(x, y, c);
    }
    void Rect(int x0, int y0, int x1, int y1, Color c) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) Set(x, y, c);
    }
};

// --- 5x7 ビットマップフォント（必要なグリフのみ）------------------------------
// 各グリフ 7 行、下位 5 ビットが 1 行（bit4=左端）。
struct Glyph { uint8_t rows[7]; };

const Glyph& FindGlyph(char ch) {
    // 数字 0-9 と J,Q,K,A のみ用意（トランプのランク表記に必要な文字）。
    static const Glyph g0  = { { 0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110 } };
    static const Glyph g1  = { { 0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110 } };
    static const Glyph g2  = { { 0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111 } };
    static const Glyph g3  = { { 0b11111,0b00010,0b00100,0b00010,0b00001,0b10001,0b01110 } };
    static const Glyph g4  = { { 0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010 } };
    static const Glyph g5  = { { 0b11111,0b10000,0b11110,0b00001,0b00001,0b10001,0b01110 } };
    static const Glyph g6  = { { 0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110 } };
    static const Glyph g7  = { { 0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000 } };
    static const Glyph g8  = { { 0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110 } };
    static const Glyph g9  = { { 0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100 } };
    static const Glyph gJ  = { { 0b00111,0b00010,0b00010,0b00010,0b10010,0b10010,0b01100 } };
    static const Glyph gQ  = { { 0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101 } };
    static const Glyph gK  = { { 0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001 } };
    static const Glyph gA  = { { 0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } };
    static const Glyph gBlank = { { 0,0,0,0,0,0,0 } };
    switch (ch) {
        case '0': return g0; case '1': return g1; case '2': return g2;
        case '3': return g3; case '4': return g4; case '5': return g5;
        case '6': return g6; case '7': return g7; case '8': return g8;
        case '9': return g9;
        case 'J': return gJ; case 'Q': return gQ; case 'K': return gK;
        case 'A': return gA;
        default:  return gBlank;
    }
}

// 1 文字を (x,y) に scale 倍で描く。戻り値 = 描いた幅（次の文字の開始 x に使う）。
int DrawChar(Canvas& c, char ch, int x, int y, int scale, Color col) {
    const Glyph& g = FindGlyph(ch);
    for (int row = 0; row < 7; ++row) {
        for (int bit = 0; bit < 5; ++bit) {
            if (g.rows[row] & (1 << (4 - bit))) {
                c.Rect(x + bit * scale, y + row * scale,
                       x + bit * scale + scale, y + row * scale + scale, col);
            }
        }
    }
    return 5 * scale + scale; // 5px 幅 + 1px 分の字間
}

// 文字列を左から順に描く。
void DrawText(Canvas& c, const char* s, int x, int y, int scale, Color col) {
    int cx = x;
    for (const char* p = s; *p; ++p) cx += DrawChar(c, *p, cx, y, scale, col);
}

// --- スート記号（正規化座標 u,up ∈ [-1,1] での陰関数テスト）------------------
// up は「上向き正」。中心 (cx,cy)、半径 s のボックス内を走査して塗る。
bool InsideSuit(int suit, float u, float up) {
    switch (suit) {
        case 1: { // ダイヤ（菱形）
            return (std::fabs(u) + std::fabs(up)) <= 0.95f;
        }
        case 0: { // ハート（葉が上・尖りが下）
            const float X = u * 1.15f;
            const float Y = up * 1.15f;
            const float t = (X * X + Y * Y - 1.0f);
            return (t * t * t - X * X * Y * Y * Y) <= 0.0f;
        }
        case 3: { // スペード（ハートを上下反転＝尖りが上）＋下部の柄
            const float X = u * 1.15f;
            const float Y = -up * 1.15f;              // 反転
            const float t = (X * X + Y * Y - 1.0f);
            const bool body = (t * t * t - X * X * Y * Y * Y) <= 0.0f;
            // 柄（下に広がる台形）
            const bool stem = (up < -0.15f) &&
                              (std::fabs(u) < 0.12f + (-up - 0.15f) * 0.55f);
            return body || stem;
        }
        default: { // クラブ（3 円＋柄）
            auto disc = [&](float ox, float oy, float r) {
                const float dx = u - ox, dy = up - oy;
                return (dx * dx + dy * dy) <= r * r;
            };
            const bool circles = disc(0.0f, 0.45f, 0.42f) ||
                                 disc(-0.45f, -0.10f, 0.42f) ||
                                 disc(0.45f, -0.10f, 0.42f);
            const bool stem = (up < -0.10f) &&
                              (std::fabs(u) < 0.12f + (-up - 0.10f) * 0.55f);
            return circles || stem;
        }
    }
}

// スート記号を (cx,cy) 中心・半径 s で塗る。
void DrawSuit(Canvas& c, int suit, int cx, int cy, int s, Color col) {
    for (int y = cy - s; y <= cy + s; ++y) {
        for (int x = cx - s; x <= cx + s; ++x) {
            const float u  = static_cast<float>(x - cx) / static_cast<float>(s);
            const float up = static_cast<float>(cy - y) / static_cast<float>(s); // 上向き正
            if (InsideSuit(suit, u, up)) c.Set(x, y, col);
        }
    }
}

// ランク index(0..12) → 表記文字列。
const char* RankStr(int rank) {
    static const char* n[13] = { "2","3","4","5","6","7","8","9","10","J","Q","K","A" };
    return (rank >= 0 && rank < 13) ? n[rank] : "?";
}

Color SuitColor(int suit) {
    // ハート/ダイヤ＝赤、クラブ/スペード＝黒。
    return (suit == 0 || suit == 1) ? Color{ 200, 30, 30, 255 }
                                    : Color{ 20, 20, 20, 255 };
}

} // namespace

void RenderFace(int rank, int suit, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(kFaceW) * kFaceH * 4, 0);
    Canvas c{ kFaceW, kFaceH, out };

    const Color white{ 250, 248, 242, 255 };
    const Color frame{ 40, 40, 55, 255 };
    const Color col = SuitColor(suit);

    // 枠付き白背景（外周 3px を枠色に）
    c.Fill(frame);
    c.Rect(3, 3, kFaceW - 3, kFaceH - 3, white);

    // 中央の大きなスート記号
    DrawSuit(c, suit, kFaceW / 2, kFaceH / 2, 34, col);

    // 左上のランク＋小スート
    const char* rs = RankStr(rank);
    DrawText(c, rs, 8, 8, 3, col);
    DrawSuit(c, suit, 15, 44, 9, col);

    // 右下のランク（上下逆さにはしないが、四隅の情報量を上げる）
    const int rw = static_cast<int>(std::strlen(rs)) * (5 * 3 + 3);
    DrawText(c, rs, kFaceW - 8 - rw, kFaceH - 8 - 21, 3, col);
    DrawSuit(c, suit, kFaceW - 15, kFaceH - 44, 9, col);
}

void RenderBack(std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(kFaceW) * kFaceH * 4, 0);
    Canvas c{ kFaceW, kFaceH, out };

    const Color deep{ 25, 35, 110, 255 };   // 濃紺
    const Color light{ 70, 95, 190, 255 };   // 明るい青
    const Color frame{ 235, 235, 245, 255 }; // 白枠

    c.Fill(frame);
    c.Rect(4, 4, kFaceW - 4, kFaceH - 4, deep);

    // 斜め格子（|x+y| と |x-y| の周期で明色の菱形を敷く）
    for (int y = 4; y < kFaceH - 4; ++y) {
        for (int x = 4; x < kFaceW - 4; ++x) {
            if (((x + y) % 16 < 3) || ((x - y + 512) % 16 < 3)) c.Set(x, y, light);
        }
    }
}

void RenderJoker(std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(kFaceW) * kFaceH * 4, 0);
    Canvas c{ kFaceW, kFaceH, out };

    const Color white{ 250, 248, 242, 255 };
    const Color magenta{ 190, 40, 190, 255 };

    c.Fill(magenta);
    c.Rect(4, 4, kFaceW - 4, kFaceH - 4, white);

    // 中央に大きな "J"（マゼンタ）＝ジョーカー＝ババ。
    DrawText(c, "J", kFaceW / 2 - 15, kFaceH / 2 - 30, 9, magenta);
    // 上下に小さく "JOKER" 代わりの記号列は用意した文字だけで（J のみ）目立たせる。
    DrawText(c, "J", 8, 8, 3, magenta);
    DrawText(c, "J", kFaceW - 8 - 18, kFaceH - 8 - 21, 3, magenta);
}

void RenderFelt(int width, int height, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    Canvas c{ width, height, out };

    // 緑フェルト＋微ノイズ（決定論的な簡易 LCG）。
    uint32_t seed = 0x1234567u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int>((seed >> 16) & 0x1F) - 15; // -15..+16
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int n = rnd();
            Color col{ static_cast<uint8_t>(15 + (n < 0 ? 0 : n / 4)),
                       static_cast<uint8_t>(95 + n),
                       static_cast<uint8_t>(45 + (n < 0 ? 0 : n / 4)),
                       255 };
            c.Set(x, y, col);
        }
    }
}

} // namespace cardart
