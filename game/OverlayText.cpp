#include "game/OverlayText.h"

using namespace DirectX;

namespace overlaytext {
namespace {

// 5x7 ビットマップフォント（クリア画面に必要な大文字/数字/記号のみ）。
// 各グリフ 7 行・下位 5 ビットが 1 行（bit4=左端）。未定義文字は空白。
struct Glyph { unsigned char rows[7]; };

const Glyph& Find(char ch) {
    static const Glyph blank = { { 0,0,0,0,0,0,0 } };
    static const Glyph A = { { 0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } };
    static const Glyph C = { { 0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110 } };
    static const Glyph E = { { 0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111 } };
    static const Glyph G = { { 0b01110,0b10001,0b10000,0b10111,0b10001,0b10001,0b01111 } };
    static const Glyph I = { { 0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b11111 } };
    static const Glyph L = { { 0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111 } };
    static const Glyph N = { { 0b10001,0b11001,0b10101,0b10011,0b10001,0b10001,0b10001 } };
    static const Glyph O = { { 0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110 } };
    static const Glyph P = { { 0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000 } };
    static const Glyph Q = { { 0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101 } };
    static const Glyph R = { { 0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001 } };
    static const Glyph S = { { 0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110 } };
    static const Glyph T = { { 0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100 } };
    static const Glyph U = { { 0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110 } };
    static const Glyph V = { { 0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100 } };
    static const Glyph W = { { 0b10001,0b10001,0b10001,0b10101,0b10101,0b11011,0b10001 } };
    static const Glyph Y = { { 0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100 } };
    static const Glyph M = { { 0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001 } };
    static const Glyph D = { { 0b11110,0b10001,0b10001,0b10001,0b10001,0b10001,0b11110 } };
    static const Glyph B = { { 0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110 } };
    static const Glyph H = { { 0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001 } };
    static const Glyph EQ  = { { 0b00000,0b00000,0b11111,0b00000,0b11111,0b00000,0b00000 } };
    static const Glyph EXC = { { 0b00100,0b00100,0b00100,0b00100,0b00100,0b00000,0b00100 } };
    static const Glyph COL = { { 0b00000,0b00100,0b00000,0b00000,0b00000,0b00100,0b00000 } };
    static const Glyph SL  = { { 0b00001,0b00010,0b00010,0b00100,0b01000,0b01000,0b10000 } };
    switch (ch) {
        case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
        case 'E': return E; case 'G': return G; case 'H': return H; case 'I': return I;
        case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O;
        case 'P': return P; case 'Q': return Q; case 'R': return R; case 'S': return S;
        case 'T': return T; case 'U': return U; case 'V': return V; case 'W': return W;
        case 'Y': return Y;
        case '=': return EQ; case '!': return EXC; case ':': return COL; case '/': return SL;
        default:  return blank;
    }
}

} // namespace

Mesh Build(Renderer& renderer, float aspect, const std::vector<std::string>& lines) {
    const float px  = 0.020f;          // NDC-y 1 フォントピクセル分
    const float pxx = px / aspect;     // x はアスペクト補正（正方形に）
    const float charW = 6.0f * pxx;    // 5px + 1px 字間
    const float lineH = 9.0f * px;     // 7px + 2px 行間
    const XMFLOAT3 nrm{ 0.0f, 0.0f, -1.0f };
    const XMFLOAT4 textCol { 1.0f, 0.95f, 0.35f, 1.0f };  // 黄
    const XMFLOAT4 panelCol{ 0.04f, 0.05f, 0.09f, 1.0f }; // 暗いパネル

    std::vector<Vertex>   v;
    std::vector<uint32_t> idx;
    auto addQuad = [&](float x0, float y0, float x1, float y1, const XMFLOAT4& c) {
        const uint32_t b = static_cast<uint32_t>(v.size());
        v.push_back(Vertex{ { x0, y0, 0.0f }, nrm, c });
        v.push_back(Vertex{ { x1, y0, 0.0f }, nrm, c });
        v.push_back(Vertex{ { x1, y1, 0.0f }, nrm, c });
        v.push_back(Vertex{ { x0, y1, 0.0f }, nrm, c });
        idx.push_back(b + 0); idx.push_back(b + 1); idx.push_back(b + 2);
        idx.push_back(b + 0); idx.push_back(b + 2); idx.push_back(b + 3);
    };

    // パネルサイズ（最長行＋余白）。
    size_t maxChars = 1;
    for (const std::string& s : lines) maxChars = (s.size() > maxChars) ? s.size() : maxChars;
    const float panelW = maxChars * charW + 8.0f * pxx;
    const float panelH = lines.size() * lineH + 6.0f * px;
    addQuad(-panelW * 0.5f, -panelH * 0.5f, panelW * 0.5f, panelH * 0.5f, panelCol);

    // 各行を中央寄せで縦に並べる（上→下）。
    const float topY = (static_cast<float>(lines.size()) - 1.0f) * lineH * 0.5f;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& s = lines[li];
        const float lineW  = s.size() * charW;
        const float startX = -lineW * 0.5f;
        const float baseY  = topY - static_cast<float>(li) * lineH; // 行の上端
        for (size_t ci = 0; ci < s.size(); ++ci) {
            const Glyph& g = Find(s[ci]);
            for (int row = 0; row < 7; ++row) {
                for (int bit = 0; bit < 5; ++bit) {
                    if (g.rows[row] & (1 << (4 - bit))) {
                        const float gx = startX + ci * charW + bit * pxx;
                        const float gy = baseY - row * px;
                        addQuad(gx, gy - px, gx + pxx, gy, textCol);
                    }
                }
            }
        }
    }

    return renderer.CreateMesh(v, idx);
}

} // namespace overlaytext
