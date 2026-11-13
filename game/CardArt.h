#pragma once

#include <cstdint>
#include <vector>

// トランプ絵柄の手続き生成（PROMPT-G-01）。
// テクスチャ画像ファイルを用意せず、CPU 上で RGBA8 ビットマップを描いて
// Renderer::CreateTexture に渡す。数字（ランク）とスート記号（♥♦♣♠）を
// 5x7 ビットマップフォント＋簡易ラスタライズで描く。
//
// 出力は常に「幅×高さ×4（RGBA, 行優先, 上→下）」バイトの vector。
namespace cardart {

constexpr int kFaceW = 128;  // カード表テクスチャの幅
constexpr int kFaceH = 176;  // カード表テクスチャの高さ（トランプ比 ≒ 2.5:3.5）

// 表カード。rank 0..12（"2".."10","J","Q","K","A"）, suit 0..3（H,D,C,S）。
void RenderFace(int rank, int suit, std::vector<uint8_t>& out);

// 裏面（青の格子模様）。
void RenderBack(std::vector<uint8_t>& out);

// ジョーカー（マゼンタ地に "J"）。
void RenderJoker(std::vector<uint8_t>& out);

// カジノ卓のフェルト（緑＋微ノイズ）。任意サイズ。
void RenderFelt(int width, int height, std::vector<uint8_t>& out);

} // namespace cardart
