#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

#include "engine/network/Packets.h"
#include "engine/network/UdpSocket.h"

#include <DirectXMath.h>

// 受信パケットを型に依らず生バイトで保持する箱
struct ReceivedPacket {
    PacketType            type;
    std::vector<uint8_t>  bytes; // ヘッダ込みの生データ
};

// クライアント側のネットワーク管理。
// 別スレッドで受信ループを回し、受信パケットをキューに積む。
class NetworkManager {
public:
    ~NetworkManager();

    // サーバーへ接続（UDP なので実際には送信先設定）。失敗してもゲームは続行可。
    bool Connect(const char* serverIp, uint16_t serverPort);
    void Disconnect();

    bool IsConnected() const { return m_connected; }

    // プレイヤー座標を送信（CRC32 付き）
    void SendPlayerState(uint32_t playerId,
                         const DirectX::XMFLOAT3& pos,
                         float yaw, float pitch);

    void SendSecurityAlert(uint32_t playerId, uint32_t alertCode, uint64_t timestamp);

    // 受信キューから 1 件取り出す（無ければ nullopt）
    std::optional<ReceivedPacket> GetNextPacket();

private:
    void ReceiveLoop();

    UdpSocket            m_socket;
    std::thread          m_thread;
    std::atomic<bool>    m_running{ false };
    std::atomic<bool>    m_connected{ false };
    std::atomic<uint32_t> m_sequence{ 0 };

    std::mutex                   m_queueMutex;
    std::queue<ReceivedPacket>   m_queue;
};
