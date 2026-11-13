// ===========================================================================
// cheat_tester — メモリスキャン型チートの検証ツール（PROMPT-V3-06 / ステージC）
//
// SECURITY NOTE: このコードは自作ゲームエンジン (GameEngine.exe) の
// プロセスに対してのみ使用する検証用ツールです。
// 他のプロセスへの使用は不正アクセス禁止法に違反する可能性があります。
//
// 何をするか:
//   起動中の GameEngine.exe のメモリを ReadProcessMemory で走査し、
//   署名(マジック値)を手掛かりに「クライアントが知るべきでない情報」を読み出す。
//     - カードモード : 相手の伏せ札（透視チート）
//     - かくれんぼ   : 全 hider の座標（ESP / 壁越し位置チート）
//     - FPS          : タイプA ウォールハック（--wallhack）＝深度制御帯を発見し書換
//   これは「全データがクライアントに在る素朴実装は破れる」ことの実証であり、
//   ステージF（サーバー権威）で同じスキャンをしても署名が見つからなくなる＝
//   原理的に防げる、という対比のためのもの。
//
//   FPS ウォールハック（--wallhack）について（typeA_wallhack_explained.md 準拠）:
//     実物の深度バッファは GPU(VRAM) 上で ReadProcessMemory では読めない。そこで
//     ゲームは「敵ドローの深度モードを決める CPU 側フラグ」(RenderControlBlock) を
//     メモリに持つ。本ツールはその署名を走査で発見して中身をテキスト表示（メモリ探索の
//     提示）し、WriteProcessMemory で depthCheatFlag を非0 に書換える＝「外部から深度
//     ステートをすり替える」。ゲームはこのフラグを読むだけでチートを知らない。防御 ON
//     （ゲーム内 P）だとブロックが毎フレーム移動して書換が空振りする。
//     ※ 段[1]〜[4]（注入・VMTフック）の武器化コードは doc 同様、意図的に非実装。
// ===========================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>   // offsetof
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/ConsoleUtf8.h"
#include "game/SecretSignatures.h"

namespace {

const char* RankName(uint8_t r) {
    static const char* n[13] = { "2","3","4","5","6","7","8","9","10","J","Q","K","A" };
    return (r < 13) ? n[r] : "?";
}
const char* SuitName(uint8_t s) {
    static const char* n[4] = { "H","D","C","S" };
    return (s < 4) ? n[s] : "?";
}

// 指定名の最初のプロセス ID を返す（見つからなければ 0）
DWORD FindPid(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lstrcmpiW(pe.szExeFile, exeName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// 対象プロセスの読み取り可能領域を走査し、magic を先頭に持つ Block を探す。
// 見つかったら out にコピーし、outRemoteAddr にゲームプロセス内のアドレスを返す
// （WriteProcessMemory の書換対象アドレスに使う）。
template <class Block>
bool FindBlock(HANDLE proc, uint64_t magic, Block& out,
               void** outRemoteAddr = nullptr) {
    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t* addr = nullptr;
    std::vector<uint8_t> buf;

    while (VirtualQueryEx(proc, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t* base = static_cast<uint8_t*>(mbi.BaseAddress);
        const SIZE_T size = mbi.RegionSize;

        const DWORD readMask = PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        const bool readable =
            (mbi.State == MEM_COMMIT) &&
            (mbi.Protect & readMask) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));

        // 巨大な予約領域などは飛ばす（実データのヒープ/スタックは十分小さい）
        if (readable && size <= (256ull << 20)) {
            buf.resize(size);
            SIZE_T got = 0;
            if (ReadProcessMemory(proc, base, buf.data(), size, &got) &&
                got >= sizeof(Block)) {
                // 署名は 8 バイト境界に置かれるが、安全側に 4 バイト刻みで走査
                for (SIZE_T off = 0; off + sizeof(Block) <= got; off += 4) {
                    uint64_t m = 0;
                    memcpy(&m, buf.data() + off, sizeof(m));
                    if (m == magic) {
                        memcpy(&out, buf.data() + off, sizeof(Block));
                        if (outRemoteAddr) *outRemoteAddr = base + off;
                        return true;
                    }
                }
            }
        }

        addr = base + size;
        if (addr < base) break; // アドレス空間の終端でのオーバーフロー保護
    }
    return false;
}

// メモリ探索の「提示」用: 見つけたブロックの生バイトを 16 進でダンプする。
void HexDump(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i += 16) {
        printf("     +%02zu: ", i);
        for (size_t j = 0; j < 16 && i + j < size; ++j) printf("%02X ", p[i + j]);
        printf("\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    EnableConsoleUtf8();

    // 既定（モード指定なし）は card+hide をスキャン（後方互換）。いずれかを明示すると
    // そのモードだけを実行する。--wallhack は FPS の深度制御帯を発見＋書換える。
    bool doCard = false, doHide = false, doWall = false, once = false, anyMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--card") == 0)          { doCard = true; anyMode = true; }
        else if (std::strcmp(argv[i], "--hide") == 0)     { doHide = true; anyMode = true; }
        else if (std::strcmp(argv[i], "--wallhack") == 0) { doWall = true; anyMode = true; }
        else if (std::strcmp(argv[i], "--once") == 0)     { once = true; }
    }
    if (!anyMode) { doCard = true; doHide = true; } // 既定

    printf("==============================================================\n");
    printf(" cheat_tester — メモリスキャン検証ツール（自作 GameEngine.exe 専用）\n");
    printf(" SECURITY NOTE: 自作プロセス以外への使用は禁止。検証目的のみ。\n");
    printf("==============================================================\n");

    const DWORD pid = FindPid(L"GameEngine.exe");
    if (pid == 0) {
        printf("GameEngine.exe が見つかりません。先にゲームを起動してください。\n");
        return 1;
    }

    // 読取(VM_READ)に加え、--wallhack のフラグ書換のため書込(VM_WRITE/VM_OPERATION)権限も取る。
    HANDLE proc = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
        FALSE, pid);
    if (proc == nullptr) {
        printf("OpenProcess に失敗しました (0x%08X)。\n", GetLastError());
        return 1;
    }
    printf("GameEngine.exe (PID %lu) にアタッチしました。\n\n", pid);

    do {
        if (doCard) {
            secrets::CardSecretBlock cb{};
            if (FindBlock(proc, secrets::kCardMagic, cb)) {
                const uint32_t n = (cb.count <= secrets::kMaxHandCards)
                                       ? cb.count : secrets::kMaxHandCards;
                printf("[カード透視] 相手の伏せ札 %u 枚を読み取りました:\n", n);
                for (uint32_t i = 0; i < n; ++i) {
                    printf("   相手[%u] = %s%s\n", i,
                           RankName(cb.cards[i].rank), SuitName(cb.cards[i].suit));
                }
            } else {
                printf("[カード透視] 署名なし（card モード未起動、または保護済み）。\n");
            }
        }
        if (doHide) {
            secrets::HiderSecretBlock hb{};
            if (FindBlock(proc, secrets::kHiderMagic, hb)) {
                const uint32_t n = (hb.count <= secrets::kMaxHiders)
                                       ? hb.count : secrets::kMaxHiders;
                printf("[ESP] hider 全 %u 人の座標を読み取りました:\n", n);
                for (uint32_t i = 0; i < n; ++i) {
                    printf("   hider[%u] = (%.1f, %.1f, %.1f)\n", i,
                           hb.positions[i].x, hb.positions[i].y, hb.positions[i].z);
                }
            } else {
                printf("[ESP] 署名なし（hide モード未起動、または保護済み）。\n");
            }
        }

        if (doWall) {
            // タイプA ウォールハック: 深度制御帯(RenderControlBlock)を署名で発見し、
            // 中身をテキスト提示（メモリ探索）してから depthCheatFlag を 1 に書き換える。
            secrets::RenderControlBlock rc{};
            void* remote = nullptr;
            if (FindBlock(proc, secrets::kRenderCtrlMagic, rc, &remote)) {
                printf("[タイプA] 深度制御帯を発見: プロセス内アドレス %p（%zu バイト）\n",
                       remote, sizeof(rc));
                HexDump(&rc, sizeof(rc));
                printf("   解釈: magic=0x%016llX  depthCheatFlag=%u  reserved=%u\n",
                       static_cast<unsigned long long>(rc.magic),
                       rc.depthCheatFlag, rc.reserved);

                // depthCheatFlag のプロセス内アドレス = ブロック先頭 + オフセット。
                void* flagAddr = static_cast<uint8_t*>(remote) +
                                 offsetof(secrets::RenderControlBlock, depthCheatFlag);
                uint32_t one = 1;
                SIZE_T wrote = 0;
                if (WriteProcessMemory(proc, flagAddr, &one, sizeof(one), &wrote) &&
                    wrote == sizeof(one)) {
                    printf("[タイプA] depthCheatFlag=1 を書き込みました"
                           "（→ fps は敵 / hide は hider が深度ALWAYS で壁越し描画＝成立）。\n");
                } else {
                    printf("[タイプA] 書き込み失敗 (0x%08X)：防御 ON でブロックが移動した"
                           "／権限不足の可能性。\n", GetLastError());
                }
            } else {
                printf("[タイプA] 深度制御帯の署名なし（fps 未起動、または防御 ON で移動中"
                       "＝スキャンが追いつかない）。\n");
            }
        }

        if (!once) {
            printf("--- 2 秒ごとに再スキャン（Ctrl+C で終了）---\n"
                   "    ※ --wallhack は毎回 走査＋書換を繰り返す。防御 ON だとブロックが\n"
                   "      毎フレーム移動するため、ここでの書換は古いアドレスに落ちて空振りする。\n\n");
            Sleep(2000);
        }
    } while (!once);

    CloseHandle(proc);
    return 0;
}
