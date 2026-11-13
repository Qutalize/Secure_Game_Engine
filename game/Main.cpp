// GameEngine エントリポイント。
// 通常起動: ウィンドウを開き、選択されたゲームモードを実行する（既定: fps）。
// --mode <name>: 起動するゲームモードを選ぶ（PROMPT-V3-01。現状 fps のみ）。
// --server [port]: 検証用 UDP サーバーとして起動する（PROMPT-06）。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "engine/core/ConsoleUtf8.h"
#include "engine/core/Window.h"
#include "engine/core/Logger.h"
#include "engine/core/PakArchive.h"
#include "engine/core/RunMetrics.h"
#include "engine/security/SecurityPolicy.h"
#include "engine/security/SecurityMetrics.h"
#ifdef VERIFY_DISTRIBUTION
#include "engine/security/DistributionIntegrity.h"
#endif
#include "engine/network/CardClient.h"
#include "engine/renderer/Renderer.h"
#include "engine/network/NetworkManager.h"
#include "engine/audio/AudioPlayer.h"

#include "game/IGameMode.h"
#include "game/GameModeManager.h"
// #include "game/CheatModule.h" // [旧チート・無効化] 協調型 DLL ローダーは撤去
#include "game/modes/RangeMode.h"
#ifdef GAME_HAS_FPS
#include "game/modes/FpsMode.h"
#endif
#ifdef GAME_HAS_CARD_HIDE
#include "game/modes/CardMode.h"
#include "game/modes/HideSeekMode.h"
#endif

#include "server/Server.h"

namespace {

constexpr int      kWidth  = 1600;   // UI/HUD が見やすいよう拡大（16:9 維持）
constexpr int      kHeight = 900;
constexpr uint16_t kServerPort = 7777;

// game.pak を開いて test.txt を読み込んでコンソールへ出す（PROMPT-07 の確認）。
// モードに依存しないアセット確認なので Main 側に残す。
#ifndef GAME_RANGE_ONLY
void TryLoadPak() {
    PakArchive pak;
    if (!pak.Open("game.pak")) {
        LOG_WARN("game.pak が見つかりません（pak_builder -i assets/ -o game.pak で作成してください）");
        return;
    }
    std::vector<uint8_t> data;
    if (pak.LoadByName("assets/test.txt", data)) {
        std::string text(data.begin(), data.end());
        printf("------ assets/test.txt from game.pak ------\n%s\n-------------------------------------------\n",
               text.c_str());
    } else {
        LOG_WARN("game.pak から assets/test.txt を読めませんでした");
    }
}

#endif
int RunGame(const std::string& modeName, bool enableCheat, RunMetrics& metrics, const RangeOptions& rangeOptions, const security::Policy& policy, const cardnet::Options& cardOptions) {
    const bool rangeMode=modeName=="range";
    const bool legacyNetwork=modeName=="fps" || modeName=="hide";
    LOG_INFO("SECURITY START %s",security::Describe(policy).c_str());
    // --- ウィンドウ ---
    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    Window window;
    if (!window.Initialize(hInstance, kWidth, kHeight, L"AntiCheat Engine")) {
        return 1;
    }
    metrics.Mark("window_ready");

    // --- レンダラ ---
    Renderer renderer;
    if (!renderer.Initialize(window.GetHandle(), kWidth, kHeight, rangeMode)) {
        return 1;
    }
    metrics.Mark("renderer_ready");

    // --- オーディオ（BGM＋効果音。失敗しても続行）---
#ifndef GAME_RANGE_ONLY
    if (!rangeMode) { AudioPlayer::Get().Init(); metrics.Mark("audio_init_returned"); }

    // --- アセット（.pak）---
#if !defined(GAME_FPS_ONLY) && !defined(VERIFY_DISTRIBUTION)
    if (legacyNetwork) { TryLoadPak(); metrics.Mark("pak_check_finished"); }
#endif

    // --- ネットワーク（接続できなくてもゲームは続行）---
    NetworkManager net;
    if (legacyNetwork && net.Connect("127.0.0.1", kServerPort)) {
        LOG_INFO("サーバー %s:%u に接続しました", "127.0.0.1", kServerPort);
    } else if (legacyNetwork) {
        LOG_WARN("サーバーへ接続できませんでした。オフラインで続行します。");
    }
    if (legacyNetwork) metrics.Mark("network_init_returned");
#endif

    // [旧チート・無効化] 協調型の外部チートDLLローダー（`--cheat` で game_cheat.dll を
    //   LoadLibrary し、HostState を手渡す方式）は撤去した。ゲーム本体はチートを一切
    //   ロード/認識しない（typeA_wallhack_explained.md：ゲームが自分でチートを読み込む
    //   構図は非現実的）。チートは完全に外部の cheat_tester が担い、ゲームは無自覚な被害者。
    //   参考（旧コード）: CheatModule cheat; if (enableCheat) cheat.Load();
    (void)enableCheat; // 旧 --cheat フラグ（現在は無視）

    // --- ゲームモードの登録と選択 ---
    GameModeManager modes;
#ifdef GAME_HAS_FPS
    modes.Register(std::make_unique<FpsMode>());
#endif
#ifdef GAME_HAS_CARD_HIDE
    modes.Register(std::make_unique<CardMode>(cardOptions));
    modes.Register(std::make_unique<HideSeekMode>());
#endif
#ifdef GAME_HAS_RANGE
    modes.Register(std::make_unique<RangeMode>(rangeOptions));
#endif

    GameContext ctx;
    ctx.renderer = &renderer;
    ctx.security = policy;
#ifndef GAME_RANGE_ONLY
    ctx.net      = legacyNetwork ? &net : nullptr;
#endif
    ctx.window   = &window;
    // ctx.cheat = &cheat; // [旧チート・無効化] ゲームはチートを保持しない（nullptr のまま）
    if (!modes.Select(modeName, ctx)) {
        // 不明なモード等。Select 内でエラーログ済み。安全に後始末して終了。
#ifndef GAME_RANGE_ONLY
        net.Disconnect();
#endif
        renderer.Shutdown();
        return 1;
    }
    IGameMode* mode = modes.Current();
    metrics.SceneReady();

    LOG_INFO("【終了方法】ESC キー（要ウィンドウフォーカス）/ コントローラーの Options・PS ボタン");

    // --- 固定タイムステップ計測用タイマー ---
    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    while (window.ProcessMessages()) {
        // 同じ Windows QPC を使う外部ランチャー/Unity と比較する入口。
        // ここは更新開始時刻であり、表示完了や入力→表示遅延を測った値ではない。
        if (metrics.FrameStart()) break;
        // dt 計測
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - prev.QuadPart) /
                   static_cast<float>(freq.QuadPart);
        prev = now;
        if (!rangeMode && dt > 0.1f) dt = 0.1f; // rangeは実時間を固定tickへ積み上げる。

        // 入力フレーム更新は engine 共通処理として Main 側で行う
        window.GetInput().BeginFrame(window.GetHandle());

        // コントローラーで終了（Options / PS / タッチパッドのいずれか）。
        // ESC（キーボード）はウィンドウがフォーカスを持つ必要があるが、こちらは
        // HID 直読みなのでフォーカスに依存せず退去できる。
        const Input& in = window.GetInput();
        if (in.IsPadDown(GamepadButton::Options) || in.IsPadDown(GamepadButton::PS) ||
            in.IsPadDown(GamepadButton::Touchpad)) {
            LOG_INFO("コントローラーで終了します（Options / PS / タッチパッド）。");
            break;
        }

        // 現在のモードを駆動する（Update → Render）
        mode->Update(dt, window.GetInput());
        mode->Render(renderer);
    }

    // --- 後始末（GPU 完了を待ってから解放）---
    mode->Shutdown();
#ifndef GAME_RANGE_ONLY
    AudioPlayer::Get().Shutdown();
    net.Disconnect();
#endif
    renderer.Shutdown();
    metrics.Mark("shutdown_complete");
    LOG_INFO("終了します。");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    RunMetrics metrics;
    EnableConsoleUtf8();

    std::string modeName = "fps"; // 既定モード（既存挙動を維持）
#ifdef GAME_RANGE_ONLY
    modeName="range";
#endif
    cardnet::Options cardOptions;
    bool cardConfigSpecified=false;
    std::string securityList;
    bool securitySpecified=false;
    std::filesystem::path securityReport;
    RangeOptions rangeOptions;
    bool verifyOnly=false;
    wchar_t exePath[32768]{};
    GetModuleFileNameW(nullptr,exePath,32768);
    rangeOptions.data=std::filesystem::path(exePath).parent_path()/"data/range.json";
    if (!std::filesystem::exists(rangeOptions.data)) rangeOptions.data="comparison/data/range.json";
    bool enableCheat = false;     // --cheat で外部チートDLLの読み込みを許可
    std::filesystem::path metricsPath;
    double benchmarkSeconds = 10.0;
    double benchmarkWarmup = 2.0;
    bool benchmarkOption = false;
    bool serverRequested = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--server") == 0) serverRequested = true;
        const bool output = std::strcmp(argv[i], "--metrics") == 0;
        const bool duration = std::strcmp(argv[i], "--benchmark-seconds") == 0;
        const bool warmup = std::strcmp(argv[i], "--benchmark-warmup") == 0;
        if (!output && !duration && !warmup) continue;
        benchmarkOption = true;
        if (++i >= argc) { fprintf(stderr, "Missing benchmark option value\n"); return 2; }
        if (output) { metricsPath = std::filesystem::absolute(argv[i]); continue; }
        char* end = nullptr; // strtod が返す借用ポインタ。所有権は argv にある。
        const double value = std::strtod(argv[i], &end);
        if (end == argv[i] || *end != '\0' || !std::isfinite(value) ||
            value < (warmup ? 0.0 : 0.001) || value > 300.0) {
            fprintf(stderr, "Invalid benchmark duration (seconds, max 300)\n"); return 2;
        }
        if (duration) benchmarkSeconds = value; else benchmarkWarmup = value;
    }
    if (benchmarkOption && (serverRequested ||
        !metrics.Configure(metricsPath, benchmarkSeconds, benchmarkWarmup))) {
        fprintf(stderr, "Benchmark requires --metrics PATH and game mode (not --server)\n");
        return 2;
    }
    if(serverRequested && (std::strcmp(argv[1],"--server")!=0 || argc>3 ||
        (argc==3 && (argv[2][0]<'0' || argv[2][0]>'9')))) {
        fprintf(stderr,"Legacy --server [port] cannot be combined with game/security options\n");return 2;
    }
#ifdef _DEBUG
    metrics.Mark("build_debug");
#else
    metrics.Mark("build_release");
#endif
#ifdef HARDENED_BUILD
    metrics.Mark("defaults_hardened");
#else
    metrics.Mark("defaults_normal");
#endif
    metrics.Mark("vsync_on");

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--metrics") == 0 ||
            std::strcmp(argv[i], "--benchmark-seconds") == 0 ||
            std::strcmp(argv[i], "--benchmark-warmup") == 0) {
            ++i; // 上で検証済みの計測引数を読み飛ばす。
        } else if (std::strcmp(argv[i], "--security") == 0 || std::strcmp(argv[i], "--security-report") == 0 ||
                   std::strcmp(argv[i], "--card-server") == 0 || std::strcmp(argv[i], "--card-seat") == 0 ||
                   std::strcmp(argv[i], "--card-token-file") == 0 || std::strcmp(argv[i], "--card-cert") == 0) {
            std::string flag=argv[i];
            if(flag.rfind("--card-",0)==0)cardConfigSpecified=true;
            if(++i>=argc) { fprintf(stderr,"Missing security/card option value\n");return 2; }
            if(flag=="--security") {
                if(securitySpecified) { fprintf(stderr,"Duplicate --security\n");return 2; }
                securityList=argv[i];securitySpecified=true;
            } else if(flag=="--security-report") securityReport=argv[i];
            else if(flag=="--card-token-file") cardOptions.tokenFile=argv[i];
            else if(flag=="--card-cert") cardOptions.certificateFile=argv[i];
            else if(flag=="--card-seat") {
                std::string seat=argv[i];
                if(seat.size()!=1 || seat[0]<'0' || seat[0]>'3') { fprintf(stderr,"Seat must be 0..3\n");return 2; }
                cardOptions.seat=static_cast<uint8_t>(seat[0]-'0');
            } else {
                std::string endpoint=argv[i];auto colon=endpoint.find(':');
                if(colon==std::string::npos || colon==0) { fprintf(stderr,"Expected numeric IPv4:port\n");return 2; }
                std::string port=endpoint.substr(colon+1);char* end=nullptr;
                unsigned long value=std::strtoul(port.c_str(),&end,10);
                if(port.empty() || *end || value==0 || value>65535) { fprintf(stderr,"Invalid card port\n");return 2; }
                cardOptions.host=endpoint.substr(0,colon);cardOptions.port=static_cast<uint16_t>(value);
            }
        } else if (std::strcmp(argv[i], "--verify-distribution") == 0) {
#ifdef VERIFY_DISTRIBUTION
            verifyOnly=true;
#else
            fprintf(stderr,"This build does not enforce a signed distribution\n");return 2;
#endif
        } else if (std::strcmp(argv[i], "--card-autoplay") == 0) {
            cardConfigSpecified=true;
            cardOptions.autoplay=true;
        } else if (std::strcmp(argv[i], "--range-data") == 0 || std::strcmp(argv[i], "--replay") == 0 ||
                   std::strcmp(argv[i], "--state-out") == 0) {
            const std::string flag=argv[i];
            if (++i>=argc) { fprintf(stderr,"Missing range option value\n"); return 2; }
            if (flag=="--range-data") rangeOptions.data=argv[i];
            else if (flag=="--replay") rangeOptions.replay=argv[i];
            else rangeOptions.stateOutput=argv[i];
        } else if (std::strcmp(argv[i], "--exit-after-replay") == 0) {
            rangeOptions.exitAfterReplay=true;
        } else if (std::strcmp(argv[i], "--server") == 0) {
#if !defined(GAME_RANGE_ONLY) && !defined(VERIFY_DISTRIBUTION)
            uint16_t port = kServerPort;
            if (i + 1 < argc) {
                char* end=nullptr;unsigned long p=std::strtoul(argv[i+1],&end,10);
                if(*end || p==0 || p>65535) { fprintf(stderr,"Invalid legacy server port\n");return 2; }
                port=static_cast<uint16_t>(p);
            }
            return RunServer(port);
#else
            fprintf(stderr,"Legacy server is not available in this profile or signed distribution\n"); return 2;
#endif
        } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            modeName = argv[i + 1];
            ++i; // 値を消費
        } else if (std::strcmp(argv[i], "--cheat") == 0) {
            // [旧チート・無効化] --cheat は受理するが何もしない（後方互換のため残置）。
            // チートは外部 cheat_tester が担うので、ゲーム起動コマンドでの切替は不要。
            enableCheat = true;
        } else { fprintf(stderr,"Unknown or incomplete argument: %s\n",argv[i]);return 2; }
    }

    security::Policy policy;
    try {
        bool hardened=false;
#ifdef HARDENED_BUILD
        hardened=true;
#endif
        const bool remote=!cardOptions.host.empty();
        policy=securitySpecified?security::Parse(securityList):security::Defaults(modeName,remote,hardened);
        security::Validate(policy,modeName,remote);
        if(remote && (cardOptions.tokenFile.empty() || !std::filesystem::is_regular_file(cardOptions.tokenFile)))
            throw std::invalid_argument("Remote card requires an existing --card-token-file for your seat");
        if(remote && (cardOptions.certificateFile.empty() || !std::filesystem::is_regular_file(cardOptions.certificateFile)))
            throw std::invalid_argument("Remote card requires a trusted --card-cert server.der file");
        if(!remote && cardConfigSpecified)
            throw std::invalid_argument("Card credentials/autoplay require --card-server");
#ifdef GAME_FPS_ONLY
        if(modeName!="fps") throw std::invalid_argument("This profile only includes fps");
#endif
#ifdef GAME_RANGE_ONLY
        if(modeName!="range") throw std::invalid_argument("This profile only includes range");
#endif
        security::EnableMetrics(!securityReport.empty());
    } catch(const std::exception& ex) { fprintf(stderr,"Configuration rejected: %s\n",ex.what());return 2; }
    int result=1;
    try {
#ifdef VERIFY_DISTRIBUTION
        const auto root=distribution::ExecutableDirectory();
        distribution::Guard verified(root);
        if(!verified.Contains(std::filesystem::path(exePath)))throw std::runtime_error("Running executable is not in signed inventory");
        if(!securityReport.empty())securityReport=std::filesystem::absolute(securityReport);
        if(!cardOptions.tokenFile.empty())cardOptions.tokenFile=std::filesystem::absolute(cardOptions.tokenFile);
        if(!cardOptions.certificateFile.empty())cardOptions.certificateFile=std::filesystem::absolute(cardOptions.certificateFile);
        rangeOptions.data=std::filesystem::absolute(rangeOptions.data);
        if(!rangeOptions.replay.empty())rangeOptions.replay=std::filesystem::absolute(rangeOptions.replay);
        if(!rangeOptions.stateOutput.empty())rangeOptions.stateOutput=std::filesystem::absolute(rangeOptions.stateOutput);
        if(modeName=="range"&&!verified.Contains(rangeOptions.data))throw std::runtime_error("Range data must be part of the signed distribution");
        metrics.Mark("distribution_verified");
        printf("DISTRIBUTION VERIFIED files=%zu bytes=%llu\n",verified.FileCount(),static_cast<unsigned long long>(verified.Bytes()));
        if(verifyOnly)return 0;
        // World paths are relative: never let a launcher's working directory choose different assets.
        std::filesystem::current_path(root);
#endif
        result=RunGame(modeName, enableCheat, metrics, rangeOptions, policy, cardOptions);
    }
    catch (const std::exception& ex) { fprintf(stderr,"Game failed: %s\n",ex.what()); }
    if (!security::SaveMetrics(securityReport)) { fprintf(stderr,"Security report save failed\n");return 3; }
    if (!metrics.Save()) {
        fprintf(stderr, "Metrics CSV could not be saved, or sample buffer overflowed\n");
        return 3;
    }
    return result;
}
