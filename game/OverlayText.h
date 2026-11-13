#pragma once

#include <string>
#include <vector>

#include "engine/renderer/Renderer.h"

// 2D オーバーレイのテキスト画面を作る（PROMPT: クリア時の GUI 画面遷移）。
// 5x7 ビットマップフォントで文字を NDC の小さな四角形として並べ、暗いパネルの上に
// 中央寄せで複数行を描く。返り値の Mesh は Renderer::DrawOverlay で最前面に描画する。
// テクスチャ/新規 PSO 不要（既存のオーバーレイ経路を流用）。
namespace overlaytext {

// lines を中央寄せで描いたオーバーレイ Mesh を作る（暗いパネル＋黄文字）。
// aspect は画面の横/縦比（Renderer::GetAspect）。文字は大文字/数字/記号(= ! : /)対応。
Mesh Build(Renderer& renderer, float aspect, const std::vector<std::string>& lines);

} // namespace overlaytext
