#pragma once

#include <cstdint>

// ===========================================================================
// CheatBridge — ゲーム本体（GameEngine.exe）と外部チートDLL（game_cheat.dll）の
// 共有インターフェース（PROMPT-H-01 / ステージH）。
//
// SECURITY NOTE: これは自作ゲーム (GameEngine.exe) の検証専用の仕組みである。
// チートを「別ファイル(game_cheat.dll)」に切り離し、`--cheat` 付き起動かつ同じ
// ディレクトリに DLL が在る場合のみ発動する。DLL が無ければチートは一切効かない。
//
// 思想: ゲームは毎フレーム「クライアントが握っている秘匿情報」(HostState) を DLL に
// 渡し、DLL は独自ホットキーで ON/OFF を判断して「何を透視表示するか」(CheatCommand)
// を返す。base のゲームコードはチートの判断ロジックを一切持たない（DLL 無し＝完全クリーン）。
//
// このヘッダは engine に依存しない純粋な C ABI 型のみ（DLL は engine とリンクしない）。
// ===========================================================================
namespace cheat {

constexpr int kApiVersion = 1;
constexpr int kMaxTargets = 32;   // ESP 対象（敵 / hider）の最大数
constexpr int kMaxCards   = 32;   // 透視するカードの最大数

struct Vec3 { float x, y, z; };

// ゲーム → DLL。毎フレーム、クライアントが握る「本来見えないはずの情報」を渡す。
// （実在のチートがメモリから盗む情報に相当。DLL に渡すことで「秘匿情報を読む」を再現）
struct HostState {
    int   mode;                          // 0=fps, 1=hide, 2=card

    // FPS の敵 / かくれんぼの hider：ワールド足元座標＋有効フラグ（生存/未発見）
    int     targetCount;
    Vec3    targets[kMaxTargets];
    uint8_t targetActive[kMaxTargets];   // 1 = ESP 対象

    // カード：相手（CPU）の手札の実値
    int     cardCount;
    uint8_t cardRank[kMaxCards];         // 0..12
    uint8_t cardSuit[kMaxCards];         // 0..3
    uint8_t cardJoker[kMaxCards];        // 1 = ジョーカー（ババ）
};

// DLL → ゲーム。何を透視表示するかの指示。
struct CheatCommand {
    bool espOn;        // true: 有効な target を壁越し ESP で描く（wallhack）
    bool revealCards;  // true: 相手の伏せ札を表向きに描く（カード透視）
};

} // namespace cheat

// game_cheat.dll がエクスポートする C ABI（名前修飾なし）。
//   CheatApiVersion(): ABI 互換確認（kApiVersion と一致すること）
//   CheatTick():       毎フレーム、in（秘匿情報）を読み out（透視指示）を書く
#if defined(GAME_CHEAT_EXPORTS)
#  define CHEAT_API __declspec(dllexport)
#else
#  define CHEAT_API __declspec(dllimport)
#endif

extern "C" {
    CHEAT_API int  CheatApiVersion();
    CHEAT_API void CheatTick(const cheat::HostState* in, cheat::CheatCommand* out);
}
