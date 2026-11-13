#include "engine/memory/LinearAllocator.h"

#include <cassert>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
// addr を alignment（2 のべき乗）境界へ切り上げる
inline size_t AlignUp(size_t value, size_t alignment) {
    return (value + (alignment - 1)) & ~(alignment - 1);
}
}

LinearAllocator::LinearAllocator(size_t capacityBytes)
    : m_capacity(capacityBytes) {
    // 仕様書 4.1: 標準 new/malloc を避け、起動時に巨大ブロックを一括確保する
    m_base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, m_capacity,
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(m_base && "VirtualAlloc failed in LinearAllocator");
}

LinearAllocator::~LinearAllocator() {
    if (m_base) {
        VirtualFree(m_base, 0, MEM_RELEASE);
        m_base = nullptr;
    }
}

void* LinearAllocator::Allocate(size_t size, size_t alignment) {
    assert((alignment & (alignment - 1)) == 0 && "alignment must be a power of two");

    size_t alignedOffset = AlignUp(m_offset, alignment);
    size_t newOffset     = alignedOffset + size;

    // 残量チェック（未定義動作を避けるため境界を越えたら停止）
    assert(newOffset <= m_capacity && "LinearAllocator out of memory");
    if (newOffset > m_capacity) return nullptr;

    void* ptr = m_base + alignedOffset;
    m_offset  = newOffset;
    return ptr;
}
