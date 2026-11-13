#pragma once
#include <cstdint>
#include <memory>
#include <filesystem>
#include <span>
#include <string>
#include <chrono>

namespace cardnet {
// Borrows the connected socket. Schannel owns cryptography; this class frames its records.
class TlsTransport {
public:
    using Deadline=std::chrono::steady_clock::time_point;
    TlsTransport();
    ~TlsTransport();
    void Connect(uintptr_t socket,const std::string& host,const std::filesystem::path& certificate);
    void Write(std::span<const uint8_t>,Deadline);
    void Read(std::span<uint8_t>,Deadline);
    void Close();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
