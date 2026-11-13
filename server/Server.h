#pragma once

#include <cstdint>

// 検証用 UDP サーバー。
// - PlayerState を受信したら他の全クライアントへ転送（エコー/リレー）
// - SecurityAlert を受信したらコンソールに警告を出力
// この関数は受信ループを回し続ける（Ctrl+C で停止）。
int RunServer(uint16_t port = 7777);
