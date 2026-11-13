#include "engine/security/IntegrityWatchdog.h"

#include <chrono>
#include "engine/security/SecurityMetrics.h"

#include "engine/security/Crc32.h"
#include "engine/core/Logger.h"

IntegrityWatchdog::IntegrityWatchdog(unsigned intervalMs)
    : m_intervalMs(intervalMs == 0 ? 1u : intervalMs) {}

IntegrityWatchdog::~IntegrityWatchdog() {
    Stop(); // 監視スレッドを確実に止めてから（Provider の参照先が壊れる前に）破棄
}

int IntegrityWatchdog::Register(std::string name, Provider provider) {
    // Start 前提なので競合しないが、安全のためロックして登録する。
    std::lock_guard<std::mutex> lk(m_mutex);
    Entry e;
    e.name     = std::move(name);
    e.provider = std::move(provider);
    m_entries.push_back(std::move(e));
    return static_cast<int>(m_entries.size()) - 1;
}

void IntegrityWatchdog::Commit(int token) {
    // 注意: 呼び出し側が AcquireLock() を保持中、または Start() 前であること。
    //       ここでロックを取らないのは、AcquireLock() 保持中に呼ばれても
    //       デッドロックさせないため（同じ非再帰 mutex の二重ロック回避）。
    if (token < 0 || token >= static_cast<int>(m_entries.size())) return;
    Entry& e = m_entries[static_cast<size_t>(token)];
    const Region r = e.provider();
    if (r.data != nullptr && r.size != 0) {
        e.expected    = security::Crc32(r.data, r.size);
        e.hasBaseline = true;
    }
}

void IntegrityWatchdog::Start() {
    if (m_running.exchange(true)) return;            // 既に動作中なら何もしない
    m_thread = std::thread(&IntegrityWatchdog::ThreadMain, this);
}

void IntegrityWatchdog::Stop() {
    if (!m_running.exchange(false)) return;          // 既に停止済みなら何もしない
    if (m_thread.joinable()) m_thread.join();
}

void IntegrityWatchdog::ThreadMain() {
    using namespace std::chrono_literals;
    LOG_INFO("[Watchdog] 整合性監視スレッド開始（%u ms 間隔 / CRC32）", m_intervalMs);

    while (m_running.load()) {
        {
            // ゲーム側の ShuffleSlots / 正規更新 / 改ざん実演と排他してチェック。
            std::lock_guard<std::mutex> lk(m_mutex);
            for (Entry& e : m_entries) {
                if (!e.hasBaseline) continue;           // まだ基準が無い領域は飛ばす
                const Region r = e.provider();
                if (r.data == nullptr || r.size == 0) continue;

                security::Measure measure(security::Work::Watchdog);
                const uint32_t now = security::Crc32(r.data, r.size);
                if (now != e.expected) {
                    LOG_WARN("[Watchdog] 改ざん検知! 領域 '%s' の CRC32 不一致 "
                             "(期待 0x%08X / 実測 0x%08X)。外部によるメモリ改変の疑い。",
                             e.name.c_str(), e.expected, now);
                    // 一度警告したら新しい値を基準に更新し、同じ改ざんでの警告連発を
                    // 防ぐ（検知の事実はログに残る）。次に元へ戻されれば再び検知する。
                    e.expected = now;
                }
            }
        }

        // intervalMs を細切れに sleep して Stop() への反応を早くする。
        for (unsigned slept = 0; slept < m_intervalMs && m_running.load(); slept += 20) {
            std::this_thread::sleep_for(20ms);
        }
    }

    LOG_INFO("[Watchdog] 整合性監視スレッド停止");
}
