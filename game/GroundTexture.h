#pragma once

#include <cstdint>
#include <vector>

// 地面（床）用テクスチャの手続き生成（PROMPT-I-02）。
// 画像ファイルを使わず、CPU でタイル可能な RGBA8 を生成して Renderer::CreateTexture に渡す。
// WRAP サンプラーで床に何枚もタイリングして「広い平面の単調さ」を軽減する。
namespace groundtex {

// 草地（緑＋ノイズ＋草の筋）。かくれんぼ場向け。
void RenderGrass(int width, int height, std::vector<uint8_t>& out);

// コンクリート/舗装（灰＋ノイズ＋目地＋斑点）。FPS 闘技場向け。
void RenderConcrete(int width, int height, std::vector<uint8_t>& out);

// レンガ壁（走り目地＋モルタル＋レンガごとの色ムラ）。arena の壁向け。タイル可能。
void RenderBrick(int width, int height, std::vector<uint8_t>& out);

// 木箱（縦板＋板の隙間＋木目＋外枠）。arena のカバー木箱向け。タイル可能。
void RenderWoodCrate(int width, int height, std::vector<uint8_t>& out);

} // namespace groundtex
