#include "engine/core/RunMetrics.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

RunMetrics::RunMetrics() : m_entry(Now()) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    m_frequency = frequency.QuadPart;
}

int64_t RunMetrics::Now() {
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

bool RunMetrics::Configure(const std::filesystem::path& path, double seconds, double warmup) {
    if (path.empty() || !std::isfinite(seconds) || !std::isfinite(warmup) ||
        seconds <= 0.0 || seconds > 300.0 || warmup < 0.0 || warmup > 300.0 ||
        m_frequency <= 0) return false;
    m_path = path;
    m_seconds = seconds;
    m_warmup = warmup;
    // 測定区間中に vector の再確保が起きないよう容量を固定する。
    m_samples.reserve(MAX_SAMPLES);
    Add("native_entry", m_entry);
    return true;
}

void RunMetrics::Add(const char* name, int64_t ticks, double intervalMs) {
    if (m_path.empty()) return;
    if (m_samples.size() == MAX_SAMPLES) { m_overflow = true; return; }
    m_samples.push_back({name, ticks, intervalMs});
}

void RunMetrics::Mark(const char* name) {
    if (!m_path.empty()) Add(name, Now());
}

void RunMetrics::SceneReady() {
    if (m_path.empty() || m_ready) return;
    Mark("scene_ready");
    m_ready = true;
}

bool RunMetrics::FrameStart() {
    if (m_path.empty() || !m_ready) return false;
    const int64_t now = Now();
    if (m_firstFrame == 0) {
        m_firstFrame = now;
        Add("first_update_begin", now);
    }
    if (m_sampleStart == 0) {
        if (static_cast<double>(now - m_firstFrame) / m_frequency >= m_warmup) {
            m_sampleStart = now;
            Add("sample_begin", now);
        }
    } else {
        // dt の上限処理より前の生の間隔。VSync/OS待ちも含み、GPU時間ではない。
        Add("update_interval", now,
            1000.0 * static_cast<double>(now - m_previous) / m_frequency);
        if (static_cast<double>(now - m_sampleStart) / m_frequency >= m_seconds) {
            Add("sample_end", now);
            return true;
        }
    }
    m_previous = now;
    return m_overflow;
}

bool RunMetrics::Save() const {
    if (m_path.empty()) return true;
    std::ofstream out(m_path);
    if (!out) return false;
    out.imbue(std::locale::classic());
    out << "schema,kind,name,qpc_ticks,qpc_frequency,value_ms\n" << std::setprecision(10);
    for (const auto& sample : m_samples) {
        out << "1," << (sample.intervalMs < 0.0 ? "event" : "frame") << ','
            << sample.name << ',' << sample.ticks << ',' << m_frequency << ',';
        if (sample.intervalMs >= 0.0) out << sample.intervalMs;
        out << '\n';
    }
    if (m_overflow) out << "1,event,buffer_overflow," << Now() << ',' << m_frequency << ",\n";
    out.flush();
    return out.good() && !m_overflow;
}
