#include "engine/renderer/PngLoader.h"

#include <cstring>

namespace {

// ===========================================================================
// DEFLATE 展開（RFC 1951）
// ===========================================================================
//
// deflate は「LZ77（過去に出た並びの繰り返しを (距離, 長さ) で参照）」と
// 「ハフマン符号（よく出る記号を短いビット列に）」の二段構えの圧縮方式。
// 展開側はビット列を読みながら、
//   ・記号 0..255       → そのままバイトを出力
//   ・記号 256          → ブロック終端
//   ・記号 257..285     → 「長さ」。続けて距離を読み、出力済みの過去データをコピー
// を繰り返すだけでよい。圧縮側（どう符号を割り当てるか）を作らないぶん、かなり短く済む。

// ビット単位で読み進めるリーダー。deflate はバイト境界に揃わないので必須。
struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos  = 0;       // 次に読むバイト位置
    uint32_t bitBuf = 0;   // 未消化のビット（下位から詰まっている）
    int      bitCnt = 0;   // bitBuf に入っている有効ビット数
    bool     overrun = false;

    // 下位ビットから n ビット取り出す（deflate はリトルエンディアン的にビットを詰める）。
    uint32_t Bits(int n) {
        while (bitCnt < n) {
            if (pos >= size) { overrun = true; return 0; }
            bitBuf |= static_cast<uint32_t>(data[pos++]) << bitCnt;
            bitCnt += 8;
        }
        const uint32_t v = bitBuf & ((1u << n) - 1u);
        bitBuf >>= n;
        bitCnt -= n;
        return v;
    }

    void AlignToByte() { bitBuf = 0; bitCnt = 0; }
};

// カノニカルハフマン木。
// 「各記号の符号長」だけから符号表が一意に決まる（= カノニカル）性質を使う。
// counts[len] = その長さの符号が何個あるか / symbols = 長さ順に並べた記号列。
struct Huffman {
    static constexpr int kMaxBits = 15;
    int counts[kMaxBits + 1] = {};
    std::vector<int> symbols;

    bool Build(const uint8_t* lengths, int n) {
        memset(counts, 0, sizeof(counts));
        for (int i = 0; i < n; ++i) {
            if (lengths[i] > kMaxBits) return false;
            ++counts[lengths[i]];
        }
        counts[0] = 0; // 長さ 0 = 使われない記号

        // 長さごとの開始位置を作り、記号を長さ順に並べる。
        int offsets[kMaxBits + 2] = {};
        for (int len = 1; len <= kMaxBits; ++len) {
            offsets[len + 1] = offsets[len] + counts[len];
        }
        symbols.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            if (lengths[i] != 0) {
                symbols[static_cast<size_t>(offsets[lengths[i]]++)] = i;
            }
        }
        return true;
    }

    // 1 記号を復号する。失敗したら -1。
    // カノニカル符号なので「1 ビットずつ読み足して、その長さの範囲に入るか」を見るだけでよい。
    int Decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>(br.Bits(1));
            if (br.overrun) return -1;
            const int count = counts[len];
            if (code - first < count) {
                return symbols[static_cast<size_t>(index + (code - first))];
            }
            index += count;
            first  = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

// 長さ記号 257..285 の「基準長」と「追加ビット数」
const uint16_t kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const uint8_t kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
// 距離記号 0..29 の「基準距離」と「追加ビット数」
const uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
const uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

bool InflateBlockData(BitReader& br, const Huffman& lit, const Huffman& dist,
                      std::vector<uint8_t>& out, std::string& err) {
    for (;;) {
        const int sym = lit.Decode(br);
        if (sym < 0) { err = "deflate: リテラル/長さ記号の復号に失敗"; return false; }
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
            continue;
        }
        if (sym == 256) return true; // ブロック終端

        const int li = sym - 257;
        if (li >= 29) { err = "deflate: 長さ記号が範囲外"; return false; }
        const int length = kLenBase[li] + static_cast<int>(br.Bits(kLenExtra[li]));

        const int dsym = dist.Decode(br);
        if (dsym < 0 || dsym >= 30) { err = "deflate: 距離記号の復号に失敗"; return false; }
        const int distance = kDistBase[dsym] + static_cast<int>(br.Bits(kDistExtra[dsym]));

        if (br.overrun) { err = "deflate: 入力が途中で尽きました"; return false; }
        if (static_cast<size_t>(distance) > out.size()) {
            err = "deflate: 参照距離が出力範囲を超えています";
            return false;
        }
        // ★1 バイトずつコピーすること★
        // distance < length のとき、コピー元とコピー先が重なる（＝直前に書いたバイトを
        // さらにコピーする）。これは deflate の正当な表現（例: 同じ 1 バイトの繰り返し）
        // なので、memcpy でまとめて動かすと結果が変わってしまう。
        size_t src = out.size() - static_cast<size_t>(distance);
        for (int i = 0; i < length; ++i) {
            out.push_back(out[src + static_cast<size_t>(i)]);
        }
    }
}

bool Inflate(BitReader& br, std::vector<uint8_t>& out, std::string& err) {
    for (;;) {
        const uint32_t isFinal = br.Bits(1);
        const uint32_t type    = br.Bits(2);
        if (br.overrun) { err = "deflate: ブロックヘッダが読めません"; return false; }

        if (type == 0) {
            // 非圧縮ブロック: バイト境界に揃えて LEN / ~LEN / 生データ。
            br.AlignToByte();
            if (br.pos + 4 > br.size) { err = "deflate: 非圧縮ブロックが壊れています"; return false; }
            const uint32_t len  = static_cast<uint32_t>(br.data[br.pos]) |
                                  (static_cast<uint32_t>(br.data[br.pos + 1]) << 8);
            const uint32_t nlen = static_cast<uint32_t>(br.data[br.pos + 2]) |
                                  (static_cast<uint32_t>(br.data[br.pos + 3]) << 8);
            br.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) { err = "deflate: LEN/~LEN が一致しません"; return false; }
            if (br.pos + len > br.size)  { err = "deflate: 非圧縮データが足りません"; return false; }
            out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
            br.pos += len;

        } else if (type == 1) {
            // 固定ハフマン: 符号長が規格で決まっている（小さいデータ向け）。
            uint8_t litLen[288];
            for (int i = 0;   i < 144; ++i) litLen[i] = 8;
            for (int i = 144; i < 256; ++i) litLen[i] = 9;
            for (int i = 256; i < 280; ++i) litLen[i] = 7;
            for (int i = 280; i < 288; ++i) litLen[i] = 8;
            uint8_t distLen[30];
            for (int i = 0; i < 30; ++i) distLen[i] = 5;

            Huffman lit, dist;
            if (!lit.Build(litLen, 288) || !dist.Build(distLen, 30)) {
                err = "deflate: 固定ハフマン表の構築に失敗"; return false;
            }
            if (!InflateBlockData(br, lit, dist, out, err)) return false;

        } else if (type == 2) {
            // 動的ハフマン: 符号表そのものがデータに埋め込まれている。
            // しかもその符号表を「符号長の符号（code length code）」で圧縮してある。
            const int hlit  = static_cast<int>(br.Bits(5)) + 257;
            const int hdist = static_cast<int>(br.Bits(5)) + 1;
            const int hclen = static_cast<int>(br.Bits(4)) + 4;
            if (br.overrun) { err = "deflate: 動的ハフマンのヘッダが読めません"; return false; }

            // code length code の符号長は、この決まった順番で格納されている。
            static const int kOrder[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };
            uint8_t clLen[19] = {};
            for (int i = 0; i < hclen; ++i) {
                clLen[kOrder[i]] = static_cast<uint8_t>(br.Bits(3));
            }
            Huffman cl;
            if (!cl.Build(clLen, 19)) { err = "deflate: code length 表の構築に失敗"; return false; }

            // リテラル表と距離表の符号長を、まとめて復号する。
            // 記号 16 = 直前の長さを 3..6 回繰り返す / 17 = 0 を 3..10 回 / 18 = 0 を 11..138 回
            const int total = hlit + hdist;
            uint8_t lengths[288 + 32] = {};
            int i = 0;
            while (i < total) {
                const int sym = cl.Decode(br);
                if (sym < 0) { err = "deflate: 符号長の復号に失敗"; return false; }
                if (sym < 16) {
                    lengths[i++] = static_cast<uint8_t>(sym);
                } else {
                    int repeat = 0;
                    uint8_t value = 0;
                    if (sym == 16) {
                        if (i == 0) { err = "deflate: 繰り返し指定の前に符号長がありません"; return false; }
                        value  = lengths[i - 1];
                        repeat = 3 + static_cast<int>(br.Bits(2));
                    } else if (sym == 17) {
                        repeat = 3 + static_cast<int>(br.Bits(3));
                    } else {
                        repeat = 11 + static_cast<int>(br.Bits(7));
                    }
                    if (i + repeat > total) { err = "deflate: 符号長の繰り返しが範囲外"; return false; }
                    for (int r = 0; r < repeat; ++r) lengths[i++] = value;
                }
            }

            Huffman lit, dist;
            if (!lit.Build(lengths, hlit) || !dist.Build(lengths + hlit, hdist)) {
                err = "deflate: 動的ハフマン表の構築に失敗"; return false;
            }
            if (!InflateBlockData(br, lit, dist, out, err)) return false;

        } else {
            err = "deflate: 未知のブロック種別(3)";
            return false;
        }

        if (isFinal) return true;
        if (br.overrun) { err = "deflate: 入力が途中で尽きました"; return false; }
    }
}

// ===========================================================================
// PNG
// ===========================================================================
uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}

int ChannelsOf(int colorType) {
    switch (colorType) {
        case 0: return 1; // グレースケール
        case 2: return 3; // RGB
        case 3: return 1; // パレット参照（インデックス 1 個）
        case 4: return 2; // グレー + アルファ
        case 6: return 4; // RGBA
        default: return 0;
    }
}

// Paeth 予測子（PNG のフィルタ 4）。左・上・左上のうち「予測値に最も近いもの」を選ぶ。
uint8_t Paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p  = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = (p > a) ? p - a : a - p;
    const int pb = (p > b) ? p - b : b - p;
    const int pc = (p > c) ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
bool ZlibInflate(const uint8_t* data, size_t size,
                 std::vector<uint8_t>& out, std::string* error) {
    std::string err;
    if (size < 2) {
        if (error) *error = "zlib: データが短すぎます";
        return false;
    }
    // zlib ヘッダ: CMF(1) + FLG(1)。CM(下位4bit)=8 が deflate。
    const uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8) {
        if (error) *error = "zlib: 圧縮方式が deflate ではありません";
        return false;
    }
    if (((static_cast<uint32_t>(cmf) << 8) | flg) % 31u != 0u) {
        if (error) *error = "zlib: ヘッダのチェックサムが不正です";
        return false;
    }
    if (flg & 0x20) {
        if (error) *error = "zlib: プリセット辞書には未対応です";
        return false;
    }

    BitReader br;
    br.data = data + 2;
    br.size = size - 2;   // 末尾 4 バイトの adler32 は読み飛ばして構わない
    out.clear();
    if (!Inflate(br, out, err)) {
        if (error) *error = err;
        return false;
    }
    return true;
}

bool DecodePng(const uint8_t* data, size_t size, PngImage& out, std::string* error) {
    out = PngImage{};
    static const uint8_t kSig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    if (size < 8 || memcmp(data, kSig, 8) != 0) {
        if (error) *error = "PNG: シグネチャが一致しません（JPEG などの別形式の可能性）";
        return false;
    }

    uint32_t width = 0, height = 0;
    int bitDepth = 0, colorType = 0, interlace = 0;
    std::vector<uint8_t> idat;      // IDAT は複数チャンクに分割され得るので連結する
    std::vector<uint8_t> palette;   // PLTE（RGB × 256）
    std::vector<uint8_t> paletteA;  // tRNS（パレットのアルファ）
    bool hasTrnsColor = false;
    uint16_t trnsR = 0, trnsG = 0, trnsB = 0;

    size_t pos = 8;
    bool sawIhdr = false;
    while (pos + 8 <= size) {
        const uint32_t len  = ReadBE32(data + pos);
        const uint8_t* type = data + pos + 4;
        const uint8_t* body = data + pos + 8;
        if (pos + 12 + len > size) {
            if (error) *error = "PNG: チャンクがファイル範囲を超えています";
            return false;
        }

        if (memcmp(type, "IHDR", 4) == 0) {
            if (len < 13) { if (error) *error = "PNG: IHDR が短すぎます"; return false; }
            width     = ReadBE32(body + 0);
            height    = ReadBE32(body + 4);
            bitDepth  = body[8];
            colorType = body[9];
            interlace = body[12];
            sawIhdr = true;

            if (width == 0 || height == 0) {
                if (error) *error = "PNG: 幅か高さが 0 です"; return false;
            }
            // 4GB 級の悪意ある寸法で確保爆発しないよう上限を設ける。
            if (width > 16384 || height > 16384) {
                if (error) *error = "PNG: 画像が大きすぎます（16384 以下にしてください）";
                return false;
            }
            if (interlace != 0) {
                if (error) *error = "PNG: インターレース(Adam7)には未対応です";
                return false;
            }
            if (ChannelsOf(colorType) == 0) {
                if (error) *error = "PNG: 未知のカラータイプです"; return false;
            }
        } else if (memcmp(type, "PLTE", 4) == 0) {
            palette.assign(body, body + len);
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (colorType == 3) {
                paletteA.assign(body, body + len);
            } else if (colorType == 0 && len >= 2) {
                hasTrnsColor = true;
                trnsR = trnsG = trnsB = static_cast<uint16_t>((body[0] << 8) | body[1]);
            } else if (colorType == 2 && len >= 6) {
                hasTrnsColor = true;
                trnsR = static_cast<uint16_t>((body[0] << 8) | body[1]);
                trnsG = static_cast<uint16_t>((body[2] << 8) | body[3]);
                trnsB = static_cast<uint16_t>((body[4] << 8) | body[5]);
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len; // 長さ(4) + 種別(4) + データ + CRC(4)
    }

    if (!sawIhdr)   { if (error) *error = "PNG: IHDR がありません"; return false; }
    if (idat.empty()){ if (error) *error = "PNG: IDAT がありません"; return false; }
    if (bitDepth != 1 && bitDepth != 2 && bitDepth != 4 && bitDepth != 8 && bitDepth != 16) {
        if (error) *error = "PNG: 未対応のビット深度です"; return false;
    }
    if (bitDepth < 8 && colorType != 3) {
        if (error) *error = "PNG: 8bit 未満はパレット画像のみ対応です"; return false;
    }
    if (colorType == 3 && palette.empty()) {
        if (error) *error = "PNG: パレット画像なのに PLTE がありません"; return false;
    }

    std::vector<uint8_t> raw;
    std::string err;
    if (!ZlibInflate(idat.data(), idat.size(), raw, &err)) {
        if (error) *error = "PNG: IDAT の展開に失敗（" + err + "）";
        return false;
    }

    const int channels = ChannelsOf(colorType);
    const int bitsPerPixel = channels * bitDepth;
    // フィルタは「1 ピクセル分だけ左」を参照する。1 ピクセル未満のときは 1 バイト単位。
    const size_t bpp = static_cast<size_t>((bitsPerPixel + 7) / 8);
    const size_t rowBytes = (static_cast<size_t>(width) * bitsPerPixel + 7) / 8;
    const size_t expected = (rowBytes + 1) * height; // 各行の先頭にフィルタ種別 1 バイト
    if (raw.size() < expected) {
        if (error) *error = "PNG: 展開後のデータが足りません（壊れている可能性）";
        return false;
    }

    // --- フィルタ解除 ---
    // PNG は行ごとに 5 種類の予測フィルタのどれかを掛けて圧縮率を上げている。
    // 展開側は「予測値を足し戻す」ことで元のバイト列に復元する。
    std::vector<uint8_t> img(rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t  filter = raw[(rowBytes + 1) * y];
        const uint8_t* src    = raw.data() + (rowBytes + 1) * y + 1;
        uint8_t*       dst    = img.data() + rowBytes * y;
        const uint8_t* prev   = (y > 0) ? (img.data() + rowBytes * (y - 1)) : nullptr;

        for (size_t x = 0; x < rowBytes; ++x) {
            const uint8_t a = (x >= bpp) ? dst[x - bpp] : 0;              // 左
            const uint8_t b = prev ? prev[x] : 0;                          // 上
            const uint8_t c = (prev && x >= bpp) ? prev[x - bpp] : 0;      // 左上
            uint8_t v = src[x];
            switch (filter) {
                case 0: break;                                      // None
                case 1: v = static_cast<uint8_t>(v + a); break;     // Sub
                case 2: v = static_cast<uint8_t>(v + b); break;     // Up
                case 3: v = static_cast<uint8_t>(v + ((a + b) >> 1)); break; // Average
                case 4: v = static_cast<uint8_t>(v + Paeth(a, b, c)); break; // Paeth
                default:
                    if (error) *error = "PNG: 未知のフィルタ種別です";
                    return false;
            }
            dst[x] = v;
        }
    }

    // --- RGBA8 へ展開 ---
    out.width  = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 255);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = img.data() + rowBytes * y;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = 0, g = 0, b = 0, a = 255;

            if (bitDepth < 8) {
                // パレット画像（1/2/4bit）。1 バイトに複数ピクセルが詰まっている。
                const uint32_t perByte = static_cast<uint32_t>(8 / bitDepth);
                const uint8_t  byte    = row[x / perByte];
                const uint32_t shift   = static_cast<uint32_t>(8 - bitDepth) -
                                         (x % perByte) * static_cast<uint32_t>(bitDepth);
                const uint32_t index   = (byte >> shift) & ((1u << bitDepth) - 1u);
                const size_t   po      = static_cast<size_t>(index) * 3;
                if (po + 2 < palette.size()) {
                    r = palette[po]; g = palette[po + 1]; b = palette[po + 2];
                }
                if (index < paletteA.size()) a = paletteA[index];
            } else {
                // ビット深度 16 のときは上位バイトだけ使う（8bit に落とす）。
                const size_t step = static_cast<size_t>(bitDepth / 8);
                const uint8_t* p  = row + (static_cast<size_t>(x) * channels) * step;
                auto ch = [&](int i) -> uint8_t { return p[static_cast<size_t>(i) * step]; };

                switch (colorType) {
                    case 0: r = g = b = ch(0); break;
                    case 2: r = ch(0); g = ch(1); b = ch(2); break;
                    case 3: {
                        const size_t po = static_cast<size_t>(ch(0)) * 3;
                        if (po + 2 < palette.size()) {
                            r = palette[po]; g = palette[po + 1]; b = palette[po + 2];
                        }
                        if (static_cast<size_t>(ch(0)) < paletteA.size()) a = paletteA[ch(0)];
                        break;
                    }
                    case 4: r = g = b = ch(0); a = ch(1); break;
                    case 6: r = ch(0); g = ch(1); b = ch(2); a = ch(3); break;
                    default: break;
                }
                // tRNS による色指定の透過（この色だけ完全透明にする）。
                if (hasTrnsColor && (colorType == 0 || colorType == 2)) {
                    const uint8_t tr = static_cast<uint8_t>(trnsR & 0xFF);
                    const uint8_t tg = static_cast<uint8_t>(trnsG & 0xFF);
                    const uint8_t tb = static_cast<uint8_t>(trnsB & 0xFF);
                    const uint8_t hr = static_cast<uint8_t>(trnsR >> 8);
                    const uint8_t hg = static_cast<uint8_t>(trnsG >> 8);
                    const uint8_t hb = static_cast<uint8_t>(trnsB >> 8);
                    const uint8_t cr = (bitDepth == 16) ? hr : tr;
                    const uint8_t cg = (bitDepth == 16) ? hg : tg;
                    const uint8_t cb = (bitDepth == 16) ? hb : tb;
                    if (r == cr && g == cg && b == cb) a = 0;
                }
            }

            const size_t o = (static_cast<size_t>(y) * width + x) * 4;
            out.rgba[o + 0] = r;
            out.rgba[o + 1] = g;
            out.rgba[o + 2] = b;
            out.rgba[o + 3] = a;
        }
    }

    out.valid = true;
    return true;
}
