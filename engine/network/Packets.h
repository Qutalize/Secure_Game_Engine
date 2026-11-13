#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

// ネットワークパケット定義。
// アライメントによる隙間を作らないため #pragma pack(1) で詰める。
#pragma pack(push, 1)

enum class PacketType : uint8_t {
    PlayerState   = 1,
    SecurityAlert = 2,
};

struct PacketHeader {
    PacketType type;
    uint16_t   payloadSize;
    uint32_t   sequence;
};

struct PlayerStatePayload {
    uint32_t playerId;
    float    posX, posY, posZ;
    float    yaw, pitch;
    uint32_t crc32;  // ヘッダ + ペイロード（crc32 自身を除く）の CRC32
};

struct SecurityAlertPayload {
    uint32_t playerId;
    uint32_t alertCode;  // 1=メモリ改ざん検知, 2=その他
    uint64_t timestamp;
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// CRC32（IEEE 802.3 多項式 0xEDB88820）の inline 実装
// ---------------------------------------------------------------------------
inline uint32_t Crc32(const void* data, size_t length, uint32_t seed = 0xFFFFFFFFu) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = seed;
    for (size_t i = 0; i < length; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88820u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// PlayerStatePayload の crc32 フィールドを除いた範囲のチェックサムを計算する。
// 送信時は埋め込み、受信時は検証に使う。
// header と payload は非連続なので一旦スタック上に詰めてから 1 回で計算する。
inline uint32_t ComputePlayerStateCrc(const PacketHeader& header,
                                      const PlayerStatePayload& payload) {
    constexpr size_t kCovered = offsetof(PlayerStatePayload, crc32);
    uint8_t buf[sizeof(PacketHeader) + kCovered];
    memcpy(buf, &header, sizeof(PacketHeader));
    memcpy(buf + sizeof(PacketHeader), &payload, kCovered);
    return Crc32(buf, sizeof(buf));
}
