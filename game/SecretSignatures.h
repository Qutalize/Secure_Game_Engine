#pragma once

#include <cstdint>

// ===========================================================================
// 検証用シグネチャ＆秘匿データのメモリ配置（ゲーム本体とチート検証ツールで共有）
//
// SECURITY NOTE: ここで定義する「秘匿ブロック」は、自作ゲーム (GameEngine.exe) の
// プロセスメモリ内に “クライアントが知るべきでない情報” が実在することを検証する
// ためのものである。tools/cheat_tester はこの署名(magic)を手掛かりに自作プロセスの
// メモリを読み取る検証専用ツールであり、他プロセスへの使用は禁止。
//
// 実在のメモリスキャン型チートは、コードやデータのパターンから独自に「署名」を作って
// 目的データの位置を特定する。本プロジェクトでは学習の焦点を ReadProcessMemory と
// 領域スキャンの機構に絞るため、目印となるマジック値を明示的に埋め込んでいる。
//
// ステージF（サーバー権威）では、これらの秘匿データはそもそもクライアントに送られず
// メモリにも存在しないため、スキャンしても署名が見つからない＝チートが原理的に不成立、
// という対比をこのヘッダ越しに確認する。
// ===========================================================================
namespace secrets {

// 8 バイトのマジック署名（ランダムに一致しにくい固定値）
constexpr uint64_t kCardMagic  = 0x5345435F43415244ULL; // "SEC_CARD" 相当
constexpr uint64_t kHiderMagic = 0x5345435F48494445ULL; // "SEC_HIDE" 相当
constexpr uint64_t kRenderCtrlMagic = 0x5345435F52454E44ULL; // "SEC_REND" 相当

constexpr int kMaxHandCards = 5;
constexpr int kMaxHiders    = 8;

// 注: パッキング指定はしない。ゲーム本体とチート検証ツールは同一コンパイラ(MSVC)で
// ビルドするため自然アラインメントで配置が一致する。先頭が uint64 magic なので
// ブロックは 8 バイト境界に置かれ、スキャンは 4/8 バイト刻みで確実に署名を拾える。

// カード 1 枚（rank 0..12, suit 0..3）。CardMode の Card と同じ並び。
struct SecretCard {
    uint8_t rank;
    uint8_t suit;
};

// 相手の伏せ札ブロック。先頭の magic を目印にスキャンする。
struct CardSecretBlock {
    uint64_t   magic;                  // = kCardMagic
    uint32_t   count;                  // 有効枚数
    SecretCard cards[kMaxHandCards];   // 伏せ札の実値
};

// hider 1 人の座標
struct SecretVec3 {
    float x, y, z;
};

// 全 hider の座標ブロック。先頭の magic を目印にスキャンする。
struct HiderSecretBlock {
    uint64_t   magic;                  // = kHiderMagic
    uint32_t   count;                  // hider 人数
    SecretVec3 positions[kMaxHiders];  // 全員の座標（未発見も含む＝ESP対象）
};

// ---------------------------------------------------------------------------
// FPS の描画制御ブロック（タイプA ウォールハックの「深度バッファ帯」CPU 側代役）。
//
// SECURITY NOTE / 設計意図:
//   実物の深度バッファは GPU(VRAM) 上にあり外部プロセスの ReadProcessMemory では
//   読めない。そこで「敵ドローの深度モードを毎フレーム決めるフラグ」を CPU 側の
//   署名付きブロックに置く。ゲーム本体はこのフラグを読むだけで、チートの存在を
//   一切知らない（無自覚な被害者）。
//
//   depthCheatFlag:
//     0    = 通常（DepthFunc=LESS＝壁より奥の敵は隠れる）
//     非0  = タイプA ウォールハック（DepthFunc=ALWAYS＝壁越しに敵が見える）
//
//   外部の cheat_tester がこの署名を走査で発見し、WriteProcessMemory で非0に
//   書き換える＝「外部から深度ステートをすり替える」。これは typeA_wallhack_
//   explained.md の段[5]（深度判定基準を ALWAYS にすり替える）を、注入/フック
//   （段[1]〜[4]・武器化コードは意図的に非実装）抜きで再現するための土台である。
//
//   防御: このブロックは ShuffleAllocator 上に置かれ、防御 ON 時は毎フレーム別
//   アドレスへ移動する。よって cheat_tester が掴んだアドレスへの書き込みは古い
//   スロットに落ちて空振りし、フラグは 0 のまま＝ウォールハック不成立。加えて
//   IntegrityWatchdog が改ざんを検知する。
// ---------------------------------------------------------------------------
struct RenderControlBlock {
    uint64_t magic;          // = kRenderCtrlMagic
    uint32_t depthCheatFlag; // 0=通常(LESS) / 非0=ウォールハック(ALWAYS)
    uint32_t reserved;       // 8バイト境界の維持（将来拡張用）
};

} // namespace secrets
