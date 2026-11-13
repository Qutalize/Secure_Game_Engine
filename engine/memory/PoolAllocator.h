#pragma once

#include <cstddef>
#include <cstdint>

// 固定サイズスロットのプールアロケータ。
// 内部はフリーリスト（単方向リンクリスト）で空きスロットを管理する。
class PoolAllocator {
public:
    PoolAllocator(size_t slotSize, size_t slotCount);
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&)            = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    void* Allocate();      // 空きスロットを 1 つ返す（無ければ nullptr）
    void  Free(void* ptr); // スロットを返却する

    size_t SlotSize()  const { return m_slotSize; }
    size_t SlotCount() const { return m_slotCount; }
    size_t UsedCount() const { return m_usedCount; }

    // ptr が指すスロットのインデックスを返す（このプール外なら SIZE_MAX）
    size_t SlotIndexOf(const void* ptr) const;
    // インデックスからスロット先頭ポインタを得る
    void*  SlotAt(size_t index) const;

private:
    // フリーリストのノード（空きスロットの先頭に埋め込む）
    struct FreeNode { FreeNode* next; };

    uint8_t*  m_base      = nullptr;
    size_t    m_slotSize  = 0;   // 実際に確保したスロットサイズ（>= 要求サイズ）
    size_t    m_slotCount = 0;
    size_t    m_usedCount = 0;
    FreeNode* m_freeList  = nullptr;
};
