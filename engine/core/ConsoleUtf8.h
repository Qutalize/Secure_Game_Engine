#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// コンソールの入出力コードページを UTF-8 に切り替える。
// ソースが /utf-8 でコンパイルされ文字列リテラルが UTF-8 のため、
// これを呼ばないと日本語環境(CP932)のコンソールで文字化けする。
inline void EnableConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
