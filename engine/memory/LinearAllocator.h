#pragma once

#include <cstddef>
#include <cstdint>

// リニアアロケータ（バンプアロケータ）。
// VirtualAlloc で確保した連続領域をポインタを進めるだけで配る。
// 個別解放はできず、Reset() で一括解放する（オフセットを 0 に戻すだけ）。
class LinearAllocator {
public:
    explicit LinearAllocator(size_t capacityBytes = 64ull * 1024 * 1024); // 既定 64MB
    ~LinearAllocator();

    LinearAllocator(const LinearAllocator&)            = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    // alignment は 2 のべき乗を想定。残量不足なら assert で停止する。
    void* Allocate(size_t size, size_t alignment = 16);

    void Reset() { m_offset = 0; }

    size_t Used()     const { return m_offset; }
    size_t Capacity() const { return m_capacity; }

private:
    uint8_t* m_base     = nullptr;
    size_t   m_capacity = 0;
    size_t   m_offset   = 0;
};
