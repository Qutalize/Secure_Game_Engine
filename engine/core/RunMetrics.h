#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

// イベント展示用の任意計測。QPC は同じ Windows PC の別プロセスでも比較できる。
// 描画/ディスク I/O の時間を混ぜないよう、測定中はメモリへ蓄積し終了時に CSV 化する。
class RunMetrics {
public:
    RunMetrics();
    bool Configure(const std::filesystem::path& path, double seconds, double warmup);
    // name は文字列リテラル限定。Sample のポインタはそれを借用し、解放しない。
    void Mark(const char* name);
    void SceneReady();
    bool FrameStart(); // 計測終了なら true（ゲームの通常の終了処理へ進む）。
    bool Save() const;

private:
    struct Sample {
        const char* name; // プログラム寿命の文字列リテラルを借用。
        int64_t ticks;
        double intervalMs; // 負数はイベント、非負はフレーム間隔。
    };
    static int64_t Now();
    void Add(const char* name, int64_t ticks, double intervalMs = -1.0);
    static constexpr size_t MAX_SAMPLES = 100000;
    std::filesystem::path m_path;
    std::vector<Sample> m_samples;
    int64_t m_entry = 0;
    int64_t m_frequency = 0;
    int64_t m_firstFrame = 0;
    int64_t m_previous = 0;
    int64_t m_sampleStart = 0;
    double m_seconds = 0.0;
    double m_warmup = 0.0;
    bool m_ready = false;
    bool m_overflow = false;
};
