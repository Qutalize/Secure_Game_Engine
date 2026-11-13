#pragma once
#include <cstdint>
#include <filesystem>
namespace security {
enum class Work { Shuffle, Watchdog, DepthGuard, TagMask, Count };
// Opt-in CPU instrumentation. Sampling adds overhead; does not measure GPU time.
void EnableMetrics(bool enable);
bool SaveMetrics(const std::filesystem::path&);
class Measure {
public:
    explicit Measure(Work work, bool active=true);
    ~Measure();
private:
    Work m_work;
    int64_t m_start=0;
};
}
