#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// ===========================================================================
// CRC-32（IEEE 802.3 / 多項式 0xEDB88320 の反転表）
//
// 整合性ウォッチドッグ（PROMPT-V3-07 / ステージD）で使う軽量チェックサム。
// 暗号学的強度は無い（衝突を意図的に作るのは容易）が、本用途は
// 「監視対象が 1 バイトでも変化したか」を毎フレーム高速に判定することなので
// これで十分。より強い保証が要るなら SHA-256 に差し替える余地を残す
// （ロードマップの「CRC32/SHA-256」のうち、まずは安価な CRC32 を採用）。
// ===========================================================================
namespace security {

// data から size バイトの CRC-32 を計算して返す。
inline uint32_t Crc32(const void* data, size_t size) {
    // 256 エントリのルックアップテーブルを初回呼び出し時のみ生成する。
    // 関数内 static は C++11 以降スレッドセーフに一度だけ初期化される
    // （複数スレッドの監視からこの関数を呼んでも安全）。
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace security
