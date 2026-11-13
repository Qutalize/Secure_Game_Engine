#include "engine/memory/PoolAllocator.h"

#include <cassert>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

PoolAllocator::PoolAllocator(size_t slotSize, size_t slotCount)
    : m_slotCount(slotCount) {
    // フリーリストノードを埋め込むため、スロットは最低でもポインタ 1 個ぶん必要。
    m_slotSize = slotSize < sizeof(FreeNode) ? sizeof(FreeNode) : slotSize;

    const size_t total = m_slotSize * m_slotCount;
    m_base = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(m_base && "VirtualAlloc failed in PoolAllocator");

    // 全スロットをフリーリストへ連結（先頭から順に）
    m_freeList = nullptr;
    for (size_t i = m_slotCount; i-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(m_base + i * m_slotSize);
        node->next = m_freeList;
        m_freeList = node;
    }
}

PoolAllocator::~PoolAllocator() {
    if (m_base) {
        VirtualFree(m_base, 0, MEM_RELEASE);
        m_base = nullptr;
    }
}

void* PoolAllocator::Allocate() {
    if (!m_freeList) return nullptr; // 満杯
    FreeNode* node = m_freeList;
    m_freeList = node->next;
    ++m_usedCount;
    return node;
}

void PoolAllocator::Free(void* ptr) {
    if (!ptr) return;
    assert(SlotIndexOf(ptr) != SIZE_MAX && "Free: pointer not from this pool");
    auto* node = reinterpret_cast<FreeNode*>(ptr);
    node->next = m_freeList;
    m_freeList = node;
    --m_usedCount;
}

size_t PoolAllocator::SlotIndexOf(const void* ptr) const {
    auto* p = static_cast<const uint8_t*>(ptr);
    if (p < m_base || p >= m_base + m_slotSize * m_slotCount) return SIZE_MAX;
    const size_t byteOffset = static_cast<size_t>(p - m_base);
    if (byteOffset % m_slotSize != 0) return SIZE_MAX; // スロット境界外
    return byteOffset / m_slotSize;
}

void* PoolAllocator::SlotAt(size_t index) const {
    assert(index < m_slotCount);
    return m_base + index * m_slotSize;
}
