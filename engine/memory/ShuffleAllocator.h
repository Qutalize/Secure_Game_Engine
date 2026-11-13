#pragma once

#include <cstdint>
#include <new>
#include <utility>
#include <vector>

#include "engine/memory/PoolAllocator.h"

class ShuffleAllocator;

// ShuffleAllocator のスロットを間接参照するハンドル。
// スロットがシャッフルで移動しても、Get() は常に最新のアドレスを返す。
// （生ポインタをゲームコードに保持させないことで、メモリスキャナに
//   固定アドレスを掴ませない＝アンチチートの基盤）
template <class T>
class Handle {
public:
    Handle() = default;
    Handle(ShuffleAllocator* owner, uint32_t id) : m_owner(owner), m_id(id) {}

    T*   Get() const;
    T*   operator->() const { return Get(); }
    T&   operator*()  const { return *Get(); }
    bool IsValid()    const { return m_owner != nullptr; }
    uint32_t Id()     const { return m_id; }

private:
    ShuffleAllocator* m_owner = nullptr;
    uint32_t          m_id    = 0;
};

// アンチチート用アロケータ。PoolAllocator をラップし、
// ShuffleSlots() を呼ぶたびに使用中スロットを別スロットへ移動させる。
class ShuffleAllocator {
public:
    ShuffleAllocator(size_t slotSize, size_t slotCount)
        : m_pool(slotSize, slotCount) {}

    // T を構築して Handle を返す。slotSize >= sizeof(T) であること。
    template <class T, class... Args>
    Handle<T> Allocate(Args&&... args) {
        void* slot = m_pool.Allocate();
        // プール枯渇は設計ミス。デバッグ時に気付けるよう assert で停止。
        if (!slot) { return Handle<T>(); }
        ::new (slot) T(std::forward<Args>(args)...);

        uint32_t id;
        if (!m_freeIds.empty()) {
            id = m_freeIds.back();
            m_freeIds.pop_back();
            m_slots[id] = slot;
        } else {
            id = static_cast<uint32_t>(m_slots.size());
            m_slots.push_back(slot);
        }
        return Handle<T>(this, id);
    }

    template <class T>
    void Free(Handle<T> handle) {
        if (!handle.IsValid()) return;
        const uint32_t id = handle.Id();
        void* slot = m_slots[id];
        if (!slot) return;
        reinterpret_cast<T*>(slot)->~T();
        ZeroSlot(slot);
        m_pool.Free(slot);
        m_slots[id] = nullptr;
        m_freeIds.push_back(id);
    }

    // 使用中の全スロットを新しいスロットへ移動し、旧スロットをゼロクリアする。
    // ハンドル（間接参照テーブル）が更新されるので外部ポインタは無効化される。
    void ShuffleSlots();

    // ハンドル ID から現在のスロットアドレスを引く（Handle 内部から呼ぶ）
    void* Resolve(uint32_t id) const { return m_slots[id]; }

    size_t LiveCount() const;

private:
    void ZeroSlot(void* slot); // SecureZeroMemory のラッパー（cpp 側で実装）

    PoolAllocator         m_pool;
    std::vector<void*>    m_slots;    // id -> 現在のスロットアドレス（nullptr = 解放済み）
    std::vector<uint32_t> m_freeIds;  // 再利用可能な id
};

// Handle::Get は ShuffleAllocator 完全定義後に実装する
template <class T>
T* Handle<T>::Get() const {
    return m_owner ? reinterpret_cast<T*>(m_owner->Resolve(m_id)) : nullptr;
}
