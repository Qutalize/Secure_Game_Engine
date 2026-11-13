// ===========================================================================
// ★[非推奨・無効化] このファイル（協調型チート DLL）はもう使われていません。
//   ゲーム本体は --cheat フラグも CheatModule ローダーも撤去したため、この DLL が
//   LoadLibrary されることはありません（コードは参照用に残置＝削除していない）。
//   現在の FPS ウォールハックは、外部の cheat_tester がメモリを自力走査して
//   RenderControlBlock.depthCheatFlag を WriteProcessMemory で書き換える非協調型
//   （typeA_wallhack_explained.md 準拠）に置き換わりました。
//   ※ この DLL は依然ビルドされますが、実行時には一切ロード/実行されません。
// ===========================================================================
// game_cheat.dll — 外部チートモジュール（PROMPT-H-02 / ステージH）
//
// SECURITY NOTE: このコードは自作ゲームエンジン (GameEngine.exe) に対してのみ
// 使用する検証用の「切り離したチート」です。他プロセスへの使用や配布は禁止。
//
// 役割: ゲーム本体が `--cheat` 付きで起動し、かつ同じディレクトリにこの DLL が
// 在るときだけ LoadLibrary で読み込まれる。読み込まれると、ゲームから毎フレーム
// 渡される「クライアントが握る秘匿情報」(HostState) を受け取り、独自ホットキー(O)で
// ON/OFF を判断して「何を透視表示するか」(CheatCommand) を返す。
// DLL が無ければ base のゲームはチートを一切行えない（＝チート無しの体験）。
//
// これは「チートは外部ファイルとして機能する」ことを明示的に示すための構成である。
// ===========================================================================
#define GAME_CHEAT_EXPORTS   // CheatBridge.h の CHEAT_API を dllexport にする
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#include "game/CheatBridge.h"

namespace {
bool g_espOn = false;  // チートの ON/OFF（DLL 内部で保持＝独自状態）
bool g_prevO = false;  // O キーの押下エッジ検出
} // namespace

// ABI バージョン（ゲーム側が kApiVersion と一致を確認する）。
extern "C" CHEAT_API int CheatApiVersion() {
    return cheat::kApiVersion;
}

// 毎フレーム呼ばれる。in（秘匿情報）を読み、out（透視指示）を書く。
extern "C" CHEAT_API void CheatTick(const cheat::HostState* in, cheat::CheatCommand* out) {
    if (out == nullptr) return;
    out->espOn = false;
    out->revealCards = false;

    // 独自ホットキー O で ON/OFF（押下エッジ）。GetAsyncKeyState は全体のキー状態。
    const bool oNow = (GetAsyncKeyState('O') & 0x8000) != 0;
    if (oNow && !g_prevO) {
        g_espOn = !g_espOn;
        if (g_espOn && in != nullptr) {
            // 秘匿情報を実際に読めていることを示す（実在チートがメモリから盗む情報に相当）。
            if (in->mode == 2) {
                printf("[game_cheat.dll] 透視 ON：相手の伏せ札 %d 枚を取得（ジョーカーも判別可）\n",
                       in->cardCount);
            } else {
                int active = 0;
                for (int i = 0; i < in->targetCount; ++i)
                    if (in->targetActive[i]) ++active;
                printf("[game_cheat.dll] ESP ON：%s %d 体を壁越し透視\n",
                       in->mode == 0 ? "敵" : "hider", active);
            }
        } else {
            printf("[game_cheat.dll] チート OFF\n");
        }
    }
    g_prevO = oNow;

    if (!g_espOn) return;

    // ON のとき: 敵/hider を壁越し ESP、カードは相手手札を透視する。
    out->espOn       = true;
    out->revealCards = true;
}
