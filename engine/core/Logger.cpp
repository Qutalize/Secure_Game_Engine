#include "engine/core/Logger.h"

#include <cstdarg>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace engine {

void LogMessage(const char* level, const char* file, int line, const char* fmt, ...) {
    // 可変長引数を整形
    char body[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    // ファイル名だけを取り出す（フルパスは冗長なので末尾の \ 以降）
    const char* shortFile = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '\\' || *p == '/') shortFile = p + 1;
    }

    char line_buf[1280];
    if(*shortFile)snprintf(line_buf, sizeof(line_buf), "[%s] %s (%s:%d)\n", level, body, shortFile, line);
    else snprintf(line_buf,sizeof(line_buf),"[%s] %s\n",level,body);

    // コンソールと VS 出力ウィンドウの両方へ
    fputs(line_buf, stdout);
    fflush(stdout);
    OutputDebugStringA(line_buf);
}

} // namespace engine
