#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace distribution {
// Keeps verified files open read-only without write/delete sharing for this process lifetime.
// The verifier itself must be trusted. This is an integrity check, not an anti-decompiler.
class Guard {
public:
    explicit Guard(const std::filesystem::path& root);
    ~Guard();
    Guard(const Guard&)=delete;
    Guard& operator=(const Guard&)=delete;
    bool Contains(const std::filesystem::path& path) const;
    size_t FileCount() const;
    uint64_t Bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
std::filesystem::path ExecutableDirectory();
}
