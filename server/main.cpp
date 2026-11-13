// standalone 検証用サーバーのエントリポイント。
// 使い方: server.exe [port]   (既定ポート 7777)

#include <cstdlib>

#include "engine/core/ConsoleUtf8.h"
#include "server/Server.h"

int main(int argc, char** argv) {
    EnableConsoleUtf8();

    uint16_t port = 7777;
    if (argc >= 2) {
        port = static_cast<uint16_t>(atoi(argv[1]));
    }
    return RunServer(port);
}
