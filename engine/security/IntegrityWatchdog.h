#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// 整合性ウォッチドッグ（PROMPT-V3-07 / ステージD：防御①＝ローカル防御）
//
// 役割:
//   「正規の更新以外で変わってはいけないクリティカルデータ」を、ゲーム本体とは
//   別スレッドから定期的に CRC32 で監視する。ゲーム側が Commit()（＝この変化は
//   正規）を宣言していないのに値が変わっていたら、外部ツール（Cheat Engine 等）
//   によるメモリ改ざんとみなして警告ログを出す。
//
// 難読化との組み合わせ:
//   監視対象は ShuffleAllocator によって毎フレーム別アドレスへ移動する。
//   よってアドレスを固定では持てず、チェックのたびに Provider コールバックから
//   「現在のアドレスとサイズ」を取得する。難読化（アドレス移動）と改ざん検知
//   （内容のCRC監視）を両立させるための設計。
//
// スレッド安全性:
//   ゲーム側がデータを移動(ShuffleSlots)・正規更新・改ざん実演する区間は
//   AcquireLock() で監視スレッドを排他する。これにより監視スレッドが
//   memcpy 途中の半端なスロットを読んで誤検知することを防ぐ。
//   std::thread + std::atomic<bool>（CLAUDE.md フェーズ4 の指定）で実装。
//
// 寿命に関する注意:
//   Provider はゲームモードの ShuffleAllocator / Handle を捕捉する。よって
//   本ウォッチドッグはそれらより先に破棄（＝後に宣言）し、Stop() で監視
//   スレッドを join してから Provider の参照先が壊れるようにすること。
// ===========================================================================
class IntegrityWatchdog {
public:
    // 監視対象の現在位置。data が null / size 0 のときはその回のチェックを飛ばす。
    struct Region {
        const void* data = nullptr;
        size_t      size = 0;
    };
    using Provider = std::function<Region()>;

    explicit IntegrityWatchdog(unsigned intervalMs = 500);
    ~IntegrityWatchdog();

    // コピー/ムーブ不可（std::thread / std::mutex を内包するため）。
    IntegrityWatchdog(const IntegrityWatchdog&) = delete;
    IntegrityWatchdog& operator=(const IntegrityWatchdog&) = delete;

    // 監視対象を登録し、トークン(index)を返す。Start() より前に呼ぶこと。
    int Register(std::string name, Provider provider);

    // 正規の更新後に期待 CRC を取り直す（＝この変化は改ざんではないと宣言）。
    // 呼び出し側は AcquireLock() を保持中、または Start() 前であること。
    // （ここで再ロックすると AcquireLock() 保持中にデッドロックするため、
    //   この関数自体はロックを取らない。）
    void Commit(int token);

    // 監視スレッドを開始/停止する。Stop() は冪等で、デストラクタからも呼ばれる。
    void Start();
    void Stop();

    // ゲーム側がクリティカルデータを移動/正規更新/改ざん実演する間に保持する
    // ロック。監視スレッドのチェックと直列化される。
    [[nodiscard]] std::unique_lock<std::mutex> AcquireLock() {
        return std::unique_lock<std::mutex>(m_mutex);
    }

private:
    void ThreadMain();

    struct Entry {
        std::string name;
        Provider    provider;
        uint32_t    expected    = 0;
        bool        hasBaseline = false;  // Commit 済みか
    };

    std::vector<Entry> m_entries;
    std::mutex         m_mutex;       // m_entries の読み書き＆ゲーム側更新を直列化
    std::thread        m_thread;
    std::atomic<bool>  m_running{ false };
    unsigned           m_intervalMs;
};
