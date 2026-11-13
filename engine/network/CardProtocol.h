#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace cardnet {
constexpr uint8_t None=255;
constexpr size_t RequestSize=40, ResponseSize=64;
enum class Operation:uint8_t { Hello=1, Poll=2, Draw=3, Restart=4 };
struct Snapshot {
    uint8_t error=0, seat=0, turn=None, victim=None, loser=None, ownCount=0;
    uint8_t discarded=0, phase=0, humans=0;
    uint64_t nextSequence=1, revision=0;
    uint32_t round=0;
    std::array<uint8_t,4> counts{};
    std::array<uint8_t,14> own{}; // No field for opponent card values.
};
std::array<uint8_t,RequestSize> Encode(Operation, uint8_t arg=0, uint64_t seq=0,
    uint64_t revision=0, const std::array<uint8_t,16>& token={});
Snapshot Decode(std::span<const uint8_t>);
}
