#pragma once

#include <cstdint>
#include <cstddef>

// 暗号化キー生成ロジック。pak_builder（暗号化）と PakArchive（復号）の
// 双方から include され、必ず同じキー／IV を導出する。
//
// 疑問点④への対応:
//   キーを文字列リテラルでハードコードしない（Ghidra で即発見されるため）。
//   ビルドタイムスタンプ定数 + ゲーム名 を FNV-1a で混ぜて動的生成する。
//   ※ 完全な隠蔽は不可能であることはフェーズ⑤で定量評価する前提。

namespace keyderiv {

// FNV-1a 32bit ハッシュ（ファイル名ハッシュにも使う）
inline uint32_t Fnv1a32(const void* data, size_t len, uint32_t seed = 2166136261u) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t hash = seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

inline uint32_t Fnv1a32Str(const char* str) {
    uint32_t hash = 2166136261u;
    for (const char* p = str; *p; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    return hash;
}

// FNV-1a 64bit（キー素材の生成に使う）
inline uint64_t Fnv1a64(const void* data, size_t len, uint64_t seed = 14695981039346656037ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// ビルドを跨いで一定の「ビルドタイムスタンプ定数」。
// （本来は CMake が生成時刻を埋め込むが、暗号化と復号で一致させる必要があるため
//   ここでは固定の定数として扱う。値を変えると過去の .pak は読めなくなる。）
constexpr uint32_t kBuildTimestamp = 0x6612A5F0u; // 任意の固定定数
constexpr char     kGameName[]     = "AntiCheatEngine";

// 16 バイト（128bit）の AES キーを導出する。
inline void DeriveKey(uint8_t outKey[16]) {
    // 4 つの 64bit 素材を異なるシードで生成し、256bit を作って先頭 128bit を使う。
    uint64_t material[4];
    uint64_t base = Fnv1a64(&kBuildTimestamp, sizeof(kBuildTimestamp));
    base = Fnv1a64(kGameName, sizeof(kGameName) - 1, base);

    material[0] = base ^ 0xA5A5A5A5A5A5A5A5ull;
    material[1] = (base * 1099511628211ull) ^ 0x0123456789ABCDEFull;
    material[2] = (base >> 17) | (base << 47);          // ローテート
    material[3] = Fnv1a64(material, sizeof(uint64_t) * 3);

    // 先頭 128bit（material[0], material[1]）を鍵として取り出す
    for (int i = 0; i < 8; ++i) {
        outKey[i]     = static_cast<uint8_t>(material[0] >> (i * 8));
        outKey[i + 8] = static_cast<uint8_t>(material[1] >> (i * 8));
    }
}

// nameHash から決定論的に 16 バイト IV を導出する。
// （ファイルごとに IV を変えることで同一平文でも暗号文が変わる。
//   IV を別途保存しなくても復号側が同じ値を再計算できる。）
inline void DeriveIv(uint32_t nameHash, uint8_t outIv[16]) {
    uint64_t a = Fnv1a64(&nameHash, sizeof(nameHash));
    uint64_t b = a ^ 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 8; ++i) {
        outIv[i]     = static_cast<uint8_t>(a >> (i * 8));
        outIv[i + 8] = static_cast<uint8_t>(b >> (i * 8));
    }
}

} // namespace keyderiv
