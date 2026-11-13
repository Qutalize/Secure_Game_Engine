#include "engine/network/UdpSocket.h"

#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

// SIO_UDP_CONNRESET は mstcpip.h にあるが、SDK 構成によっては取り込めない
// ことがあるため、未定義なら自前で定義する（_WSAIOW/IOC_VENDOR は winsock2.h）。
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "engine/core/Logger.h"

namespace {
    std::atomic<int> g_wsaRefCount{ 0 };

    sockaddr_in MakeAddr(const char* ip, uint16_t port) {
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (ip) {
            inet_pton(AF_INET, ip, &addr.sin_addr);
        } else {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        return addr;
    }
}

void UdpSocket::GlobalInit() {
    if (g_wsaRefCount.fetch_add(1) == 0) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            LOG_ERROR("WSAStartup failed: %d", WSAGetLastError());
        }
    }
}

void UdpSocket::GlobalShutdown() {
    if (g_wsaRefCount.fetch_sub(1) == 1) {
        WSACleanup();
    }
}

UdpSocket::~UdpSocket() {
    if (IsValid()) {
        closesocket(static_cast<SOCKET>(m_socket));
        m_socket = ~static_cast<uintptr_t>(0);
        GlobalShutdown();
    }
}

bool UdpSocket::IsValid() const {
    return m_socket != ~static_cast<uintptr_t>(0);
}

bool UdpSocket::Create() {
    GlobalInit();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        LOG_ERROR("socket() failed: %d", WSAGetLastError());
        GlobalShutdown();
        return false;
    }

    // ノンブロッキング化
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    // Windows 固有の罠の回避:
    // 接続先が居ない UDP へ送ると ICMP Port Unreachable が返り、
    // 次の recvfrom が WSAECONNRESET(10054) を返す。これを無効化する。
    // （サーバー未起動でもクライアントが延々とエラーを出さないようにする）
    BOOL  newBehavior   = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &newBehavior, sizeof(newBehavior),
             nullptr, 0, &bytesReturned, nullptr, nullptr);

    m_socket = static_cast<uintptr_t>(s);
    return true;
}

bool UdpSocket::Bind(uint16_t port) {
    if (!IsValid()) return false;
    sockaddr_in addr = MakeAddr(nullptr, port);
    if (bind(static_cast<SOCKET>(m_socket),
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("bind(%u) failed: %d", port, WSAGetLastError());
        return false;
    }
    LOG_INFO("UDP bound to port %u", port);
    return true;
}

bool UdpSocket::Connect(const char* ip, uint16_t port) {
    if (!IsValid()) return false;
    sockaddr_in addr = MakeAddr(ip, port);
    // UDP の connect は既定送信先の登録のみ（ハンドシェイクは無い）
    if (connect(static_cast<SOCKET>(m_socket),
                reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("connect(%s:%u) failed: %d", ip, port, WSAGetLastError());
        return false;
    }
    LOG_INFO("UDP default destination set to %s:%u", ip, port);
    return true;
}

int UdpSocket::Send(const void* data, int size) {
    if (!IsValid()) return -1;
    int sent = send(static_cast<SOCKET>(m_socket),
                    static_cast<const char*>(data), size, 0);
    return sent;
}

int UdpSocket::SendTo(const void* data, int size,
                      const std::string& ip, uint16_t port) {
    if (!IsValid()) return -1;
    sockaddr_in addr = MakeAddr(ip.c_str(), port);
    return sendto(static_cast<SOCKET>(m_socket),
                  static_cast<const char*>(data), size, 0,
                  reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
}

int UdpSocket::Receive(void* buf, int bufSize, int timeoutMs) {
    std::string ip; uint16_t port;
    return ReceiveFrom(buf, bufSize, timeoutMs, ip, port);
}

int UdpSocket::ReceiveFrom(void* buf, int bufSize, int timeoutMs,
                           std::string& outIp, uint16_t& outPort) {
    if (!IsValid()) return -1;
    SOCKET s = static_cast<SOCKET>(m_socket);

    // select でタイムアウト付きに待つ
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(s, &readSet);
    timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int sel = select(0, &readSet, nullptr, nullptr, &tv);
    if (sel <= 0) return 0; // タイムアウト or エラー

    sockaddr_in from = {};
    int fromLen = sizeof(from);
    int received = recvfrom(s, static_cast<char*>(buf), bufSize, 0,
                            reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        // WSAEWOULDBLOCK: データ無し（正常）
        // WSAECONNRESET(10054): 送信先が居ない場合の ICMP 由来。UDP では無害
        if (err != WSAEWOULDBLOCK && err != WSAECONNRESET) {
            LOG_WARN("recvfrom failed: %d", err);
        }
        return 0;
    }

    char ipStr[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &from.sin_addr, ipStr, sizeof(ipStr));
    outIp   = ipStr;
    outPort = ntohs(from.sin_port);
    return received;
}
