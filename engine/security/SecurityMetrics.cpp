#include "engine/security/SecurityMetrics.h"
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
namespace security {
namespace {
struct Counter { std::atomic<uint64_t> calls{0}, nanos{0}; };
std::array<Counter,static_cast<size_t>(Work::Count)> counters;
std::atomic<bool> enabled{false};
int64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
void EnableMetrics(bool value) { enabled.store(value); }
Measure::Measure(Work work, bool active):m_work(work) {
    if (active && enabled.load(std::memory_order_relaxed)) m_start=Now();
}
Measure::~Measure() {
    if (!m_start) return;
    auto elapsed=Now()-m_start;
    auto& c=counters[static_cast<size_t>(m_work)];
    c.calls.fetch_add(1,std::memory_order_relaxed);
    c.nanos.fetch_add(static_cast<uint64_t>(elapsed),std::memory_order_relaxed);
}
bool SaveMetrics(const std::filesystem::path& path) {
    if (path.empty()) return true;
    if (!path.parent_path().empty()) {
        std::error_code error;std::filesystem::create_directories(path.parent_path(),error);
        if(error)return false;
    }
    std::ofstream out(path);
    out<<"feature,calls,elapsed_total_ns,elapsed_mean_ns\n";
    constexpr const char* names[]={"shuffle","watchdog","depth-guard","tag-mask"};
    for (size_t i=0;i<counters.size();++i) {
        auto calls=counters[i].calls.load(), ns=counters[i].nanos.load();
        out<<names[i]<<','<<calls<<','<<ns<<','<<(calls?static_cast<double>(ns)/calls:0)<<'\n';
    }
    return static_cast<bool>(out);
}
}
