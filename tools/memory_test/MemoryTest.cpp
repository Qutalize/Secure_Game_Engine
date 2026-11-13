// アロケータの単体テスト（PROMPT-05）。
// 全テストが通れば "ALL TESTS PASSED" を出力する。

#include <cstdio>
#include <cstdint>

#include "engine/core/ConsoleUtf8.h"
#include "engine/memory/LinearAllocator.h"
#include "engine/memory/PoolAllocator.h"
#include "engine/memory/ShuffleAllocator.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
    if (cond) {
        printf("  [OK]   %s\n", what);
    } else {
        printf("  [FAIL] %s\n", what);
        ++g_failures;
    }
}

void TestLinear() {
    printf("LinearAllocator:\n");
    LinearAllocator alloc(1 * 1024 * 1024); // 1MB

    void* a = alloc.Allocate(100, 16);
    void* b = alloc.Allocate(200, 16);
    Check(a != nullptr && b != nullptr, "確保が成功する");
    Check(reinterpret_cast<uintptr_t>(a) % 16 == 0, "16 バイト境界に揃っている");
    Check(b > a, "2 つ目は 1 つ目より後ろにある");
    Check(alloc.Used() >= 300, "使用量が積算されている");

    alloc.Reset();
    Check(alloc.Used() == 0, "Reset で使用量が 0 に戻る");

    void* c = alloc.Allocate(100, 16);
    Check(c == a, "Reset 後は先頭から再利用される");
}

void TestPool() {
    printf("PoolAllocator:\n");
    PoolAllocator pool(64, 8); // 64B スロット x 8

    void* p1 = pool.Allocate();
    void* p2 = pool.Allocate();
    Check(p1 && p2 && p1 != p2, "別々のスロットが返る");
    Check(pool.UsedCount() == 2, "使用数が 2");

    pool.Free(p1);
    Check(pool.UsedCount() == 1, "Free で使用数が 1 に戻る");

    void* p3 = pool.Allocate();
    Check(p3 == p1, "解放したスロットが再利用される");

    // 枯渇テスト
    PoolAllocator small(32, 2);
    void* s1 = small.Allocate();
    void* s2 = small.Allocate();
    void* s3 = small.Allocate();
    Check(s1 && s2 && s3 == nullptr, "満杯時は nullptr を返す");
}

struct TestObj {
    uint32_t magic;
    float    value;
    int      id;
};

void TestShuffle() {
    printf("ShuffleAllocator:\n");
    ShuffleAllocator alloc(sizeof(TestObj), 16);

    Handle<TestObj> h1 = alloc.Allocate<TestObj>();
    Handle<TestObj> h2 = alloc.Allocate<TestObj>();
    h1->magic = 0xABCD1234; h1->value = 100.0f; h1->id = 1;
    h2->magic = 0x5555AAAA; h2->value = 42.5f;  h2->id = 2;

    void* before1 = h1.Get();
    void* before2 = h2.Get();
    Check(h1->value == 100.0f && h2->value == 42.5f, "確保直後に値が読める");

    alloc.ShuffleSlots();

    Check(h1.Get() != before1, "シャッフルで h1 のアドレスが変わる");
    Check(h2.Get() != before2, "シャッフルで h2 のアドレスが変わる");
    Check(h1->value == 100.0f && h1->id == 1, "シャッフル後も h1 の値が保たれる");
    Check(h2->value == 42.5f  && h2->id == 2, "シャッフル後も h2 の値が保たれる");

    // 複数回シャッフルしても整合性が保たれる
    for (int i = 0; i < 10; ++i) alloc.ShuffleSlots();
    Check(h1->magic == 0xABCD1234, "10 回シャッフル後も h1.magic が保たれる");
    Check(h2->magic == 0x5555AAAA, "10 回シャッフル後も h2.magic が保たれる");

    alloc.Free(h1);
    Check(alloc.LiveCount() == 1, "Free 後の生存数が 1");
}

} // namespace

int main() {
    EnableConsoleUtf8();
    printf("=== Memory Allocator Tests ===\n");
    TestLinear();
    TestPool();
    TestShuffle();

    printf("\n");
    if (g_failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
