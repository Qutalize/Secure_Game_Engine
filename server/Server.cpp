#include "server/Server.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "engine/network/Packets.h"
#include "engine/network/UdpSocket.h"

namespace {
// クライアントの識別キー（"ip:port"）
std::string EndpointKey(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}
}

int RunServer(uint16_t port) {
    UdpSocket sock;
    if (!sock.Create() || !sock.Bind(port)) {
        printf("[server] ポート %u の待受に失敗しました。\n", port);
        return 1;
    }
    printf("[server] UDP サーバー起動。ポート %u で待受中... (Ctrl+C で終了)\n", port);

    // 既知クライアント: key -> (ip, port)
    struct Endpoint { std::string ip; uint16_t port; };
    std::unordered_map<std::string, Endpoint> clients;

    uint8_t buf[1500];
    for (;;) {
        std::string fromIp;
        uint16_t    fromPort = 0;
        int n = sock.ReceiveFrom(buf, sizeof(buf), 200 /*ms*/, fromIp, fromPort);
        if (n < static_cast<int>(sizeof(PacketHeader))) continue;

        // 送信元を既知クライアントとして登録
        const std::string key = EndpointKey(fromIp, fromPort);
        clients[key] = Endpoint{ fromIp, fromPort };

        PacketHeader header;
        memcpy(&header, buf, sizeof(PacketHeader));

        switch (header.type) {
        case PacketType::PlayerState: {
            if (n < static_cast<int>(sizeof(PacketHeader) + sizeof(PlayerStatePayload)))
                break;
            PlayerStatePayload payload;
            memcpy(&payload, buf + sizeof(PacketHeader), sizeof(payload));

            // CRC 検証
            uint32_t expected = ComputePlayerStateCrc(header, payload);
            const char* crcStatus = (expected == payload.crc32) ? "OK" : "BAD";

            printf("[server] PlayerState id=%u pos=(%.2f, %.2f, %.2f) "
                   "yaw=%.2f pitch=%.2f seq=%u crc=%s\n",
                   payload.playerId, payload.posX, payload.posY, payload.posZ,
                   payload.yaw, payload.pitch, header.sequence, crcStatus);

            // 送信元以外の全クライアントへ転送（リレー）
            for (auto& [k, ep] : clients) {
                if (k == key) continue;
                sock.SendTo(buf, n, ep.ip, ep.port);
            }
            break;
        }
        case PacketType::SecurityAlert: {
            if (n < static_cast<int>(sizeof(PacketHeader) + sizeof(SecurityAlertPayload)))
                break;
            SecurityAlertPayload payload;
            memcpy(&payload, buf + sizeof(PacketHeader), sizeof(payload));
            printf("[server] *** SECURITY ALERT from player %u: code %u (ts=%llu) ***\n",
                   payload.playerId, payload.alertCode,
                   static_cast<unsigned long long>(payload.timestamp));
            // 仕様書 4.3: 本来はここで対象 IP のトラフィックをドロップ／ハニーポット転送
            // （XDP 連携。フェーズ4で実装）
            break;
        }
        default:
            printf("[server] 不明なパケット種別 %u\n",
                   static_cast<unsigned>(header.type));
            break;
        }
    }
    return 0;
}
