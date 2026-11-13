#include "engine/memory/ShuffleAllocator.h"

#include <cassert>
#include "engine/security/SecurityMetrics.h"
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void ShuffleAllocator::ZeroSlot(void* slot) {
    // SecureZeroMemory は最適化で消されないことが保証されたゼロクリア。
    SecureZeroMemory(slot, m_pool.SlotSize());
}

void ShuffleAllocator::ShuffleSlots() {
    security::Measure measure(security::Work::Shuffle);
    const size_t slotSize = m_pool.SlotSize();

    for (size_t id = 0; id < m_slots.size(); ++id) {
        void* oldSlot = m_slots[id];
        if (!oldSlot) continue; // 解放済み

        // 新しいスロットを確保（oldSlot はまだ解放していないので必ず別アドレス）
        void* newSlot = m_pool.Allocate();
        if (!newSlot) {
            // プールに空きが無い＝シャッフルの余地が無い。移動せず継続。
            assert(false && "ShuffleSlots: no free slot to relocate into");
            continue;
        }

        memcpy(newSlot, oldSlot, slotSize); // 内容をそのまま移送（POD 前提）
        ZeroSlot(oldSlot);                  // 旧スロットは痕跡を残さずゼロクリア
        m_pool.Free(oldSlot);

        m_slots[id] = newSlot;              // 間接参照テーブルを更新
    }
}

size_t ShuffleAllocator::LiveCount() const {
    size_t n = 0;
    for (void* s : m_slots) if (s) ++n;
    return n;
}
