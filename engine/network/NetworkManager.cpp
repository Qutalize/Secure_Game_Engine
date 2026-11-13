#include "engine/network/NetworkManager.h"

#include <cstring>

#include "engine/core/Logger.h"

using namespace DirectX;

NetworkManager::~NetworkManager() {
    Disconnect();
}

bool NetworkManager::Connect(const char* serverIp, uint16_t serverPort) {
    if (!m_socket.Create()) return false;
    if (!m_socket.Connect(serverIp, serverPort)) return false;

    m_connected = true;
    m_running   = true;
    m_thread    = std::thread(&NetworkManager::ReceiveLoop, this);
    LOG_INFO("NetworkManager connected to %s:%u", serverIp, serverPort);
    return true;
}

void NetworkManager::Disconnect() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    m_connected = false;
}

void NetworkManager::SendPlayerState(uint32_t playerId, const XMFLOAT3& pos,
                                     float yaw, float pitch) {
    if (!m_connected) return;

    PacketHeader header;
    header.type        = PacketType::PlayerState;
    header.payloadSize = sizeof(PlayerStatePayload);
    header.sequence    = m_sequence.fetch_add(1);

    PlayerStatePayload payload = {};
    payload.playerId = playerId;
    payload.posX = pos.x; payload.posY = pos.y; payload.posZ = pos.z;
    payload.yaw  = yaw;   payload.pitch = pitch;
    payload.crc32 = ComputePlayerStateCrc(header, payload);

    // ヘッダ + ペイロードを 1 つのバッファにまとめて送る
    uint8_t buf[sizeof(PacketHeader) + sizeof(PlayerStatePayload)];
    memcpy(buf, &header, sizeof(PacketHeader));
    memcpy(buf + sizeof(PacketHeader), &payload, sizeof(PlayerStatePayload));
    m_socket.Send(buf, sizeof(buf));
}

void NetworkManager::SendSecurityAlert(uint32_t playerId, uint32_t alertCode,
                                       uint64_t timestamp) {
    if (!m_connected) return;

    PacketHeader header;
    header.type        = PacketType::SecurityAlert;
    header.payloadSize = sizeof(SecurityAlertPayload);
    header.sequence    = m_sequence.fetch_add(1);

    SecurityAlertPayload payload = {};
    payload.playerId  = playerId;
    payload.alertCode = alertCode;
    payload.timestamp = timestamp;

    uint8_t buf[sizeof(PacketHeader) + sizeof(SecurityAlertPayload)];
    memcpy(buf, &header, sizeof(PacketHeader));
    memcpy(buf + sizeof(PacketHeader), &payload, sizeof(SecurityAlertPayload));
    m_socket.Send(buf, sizeof(buf));
}

std::optional<ReceivedPacket> NetworkManager::GetNextPacket() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_queue.empty()) return std::nullopt;
    ReceivedPacket pkt = std::move(m_queue.front());
    m_queue.pop();
    return pkt;
}

void NetworkManager::ReceiveLoop() {
    uint8_t buf[1500]; // MTU 程度
    while (m_running) {
        int n = m_socket.Receive(buf, sizeof(buf), 50 /*ms*/);
        if (n < static_cast<int>(sizeof(PacketHeader))) continue;

        PacketHeader header;
        memcpy(&header, buf, sizeof(PacketHeader));

        ReceivedPacket pkt;
        pkt.type = header.type;
        pkt.bytes.assign(buf, buf + n);

        std::lock_guard<std::mutex> lock(m_queueMutex);
        // キューが膨れ過ぎないよう上限を設ける
        if (m_queue.size() < 256) m_queue.push(std::move(pkt));
    }
}
