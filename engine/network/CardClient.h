#pragma once
#include "engine/network/CardProtocol.h"
#include "engine/network/TlsTransport.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace cardnet {
struct Options {
    std::string host;
    uint16_t port=7778;
    uint8_t seat=0;
    std::filesystem::path tokenFile;
    std::filesystem::path certificateFile;
    bool autoplay=false; // Explicit local test driver; server still validates every command.
};
class Connection {
public:
    ~Connection();
    Connection()=default;
    Connection(const Connection&)=delete;
    Connection& operator=(const Connection&)=delete;
    Snapshot Connect(const Options&);
    Snapshot Exchange(Operation op, uint8_t arg=0, uint64_t seq=0, uint64_t revision=0);
    void Close();
    const std::array<uint8_t,ResponseSize>& LastWire() const { return m_wire; }
private:
    Snapshot Transact(std::array<uint8_t,RequestSize>&);
    uintptr_t m_socket=~uintptr_t(0);
    bool m_wsa=false;
    uint8_t m_seat=0;
    uint64_t m_revision=0;
    std::array<uint8_t,ResponseSize> m_wire{};
    TlsTransport m_tls;
};
struct View { Snapshot state; bool connected=false, pending=false; std::string message; };
class Client {
public:
    ~Client();
    void Start(const Options&); // Bounded handshake; no offline fallback on failure.
    void Stop();
    View Read();
    bool Queue(Operation, uint8_t index, uint64_t expectedRevision);
private:
    struct Command { Operation op; uint8_t index; uint64_t revision; };
    void Run();
    Connection m_connection;
    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex;
    View m_view;
    std::optional<Command> m_command;
};
}
