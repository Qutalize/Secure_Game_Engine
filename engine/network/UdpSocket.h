#pragma once

#include <cstdint>
#include <string>

// WinSock2 を使った UDP ソケットのラッパー。
// ノンブロッキングモードで動作する。
class UdpSocket {
public:
    UdpSocket()  = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Create();                          // ソケット生成（ノンブロッキング化）
    bool Bind(uint16_t port);               // サーバー: 待受
    bool Connect(const char* ip, uint16_t port); // クライアント: 既定送信先を設定

    int  Send(const void* data, int size);  // Connect 済みの相手へ送る。送信バイト数
    // 受信。timeoutMs まで待つ。受信バイト数（タイムアウト/データ無しは 0）
    int  Receive(void* buf, int bufSize, int timeoutMs);
    // 送信元アドレスも取り出す版（サーバーがクライアントを覚えるのに使う）
    int  ReceiveFrom(void* buf, int bufSize, int timeoutMs,
                     std::string& outIp, uint16_t& outPort);

    // 受信した相手にそのまま送り返す（サーバーの転送用）
    int  SendTo(const void* data, int size, const std::string& ip, uint16_t port);

    bool IsValid() const;

private:
    // WSAStartup を 1 回だけ行うための参照カウント
    static void GlobalInit();
    static void GlobalShutdown();

    uintptr_t m_socket = ~static_cast<uintptr_t>(0); // INVALID_SOCKET 相当
};
