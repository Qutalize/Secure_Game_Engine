#include "engine/audio/AudioPlayer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>

#include <cmath>

#include "engine/core/Logger.h"

#pragma comment(lib, "xaudio2.lib")

// ===========================================================================
// XAudio2 によるオーディオ基盤（PROMPT-I-06）
//  - 全クリップ共通フォーマット: 16bit モノラル 22050Hz
//  - 効果音は SFX 用ソースボイスのプール（空きに投入＝重ねてミックス）
//  - BGM は専用ソースボイスで無限ループ
//  - 音源は手続き生成（ノイズ/サイン＋エンベロープ）
// ===========================================================================
namespace {
constexpr int kRate    = 22050;
constexpr int kSfxPool = 16;   // 同時に鳴らせる効果音の数

// サイン波 1 音（周波数 freq, 秒 dur, 音量 amp, 減衰 decay）を samples に加算。
void AddTone(std::vector<int16_t>& s, float freq, float dur, float amp, float decay) {
    const int n = static_cast<int>(kRate * dur);
    const int start = static_cast<int>(s.size());
    s.resize(static_cast<size_t>(start + n), 0);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float env = expf(-t * decay) * (1.0f - expf(-t * 80.0f)); // 立ち上がり＋減衰
        float v = sinf(6.2831853f * freq * t) * amp * env;
        int val = static_cast<int>(s[static_cast<size_t>(start + i)]) +
                  static_cast<int>(v * 30000.0f);
        if (val > 32767) val = 32767; if (val < -32768) val = -32768;
        s[static_cast<size_t>(start + i)] = static_cast<int16_t>(val);
    }
}
} // namespace

struct AudioPlayer::Impl {
    IXAudio2*               xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    WAVEFORMATEX            fmt{};
    IXAudio2SourceVoice*    sfx[kSfxPool] = {};
    IXAudio2SourceVoice*    bgmVoice = nullptr;

    // クリップ（PCM をメモリに保持＝再生中も生存させる）
    std::vector<int16_t> gunshot, hit, found, card, bgm;

    void PlayClip(const std::vector<int16_t>& clip, float vol) {
        if (xaudio == nullptr || clip.empty()) return;
        for (IXAudio2SourceVoice* v : sfx) {
            XAUDIO2_VOICE_STATE st{};
            v->GetState(&st);
            if (st.BuffersQueued == 0) {
                XAUDIO2_BUFFER buf{};
                buf.AudioBytes = static_cast<UINT32>(clip.size() * sizeof(int16_t));
                buf.pAudioData = reinterpret_cast<const BYTE*>(clip.data());
                buf.Flags      = XAUDIO2_END_OF_STREAM;
                v->SetVolume(vol);
                if (SUCCEEDED(v->SubmitSourceBuffer(&buf))) v->Start(0);
                return;
            }
        }
    }
};

AudioPlayer& AudioPlayer::Get() {
    static AudioPlayer instance;
    return instance;
}

bool AudioPlayer::Init() {
    if (m_ready) return true;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // XAudio2 は COM 前提

    m_impl = new Impl();
    if (FAILED(XAudio2Create(&m_impl->xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        LOG_WARN("XAudio2Create 失敗。音は無効になります。");
        delete m_impl; m_impl = nullptr; return false;
    }
    if (FAILED(m_impl->xaudio->CreateMasteringVoice(&m_impl->master))) {
        LOG_WARN("MasteringVoice 作成失敗。音は無効になります。");
        Shutdown(); return false;
    }

    // 共通フォーマット（16bit モノラル 22050Hz）
    WAVEFORMATEX& f = m_impl->fmt;
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 16; f.nBlockAlign = 2; f.nAvgBytesPerSec = kRate * 2;

    for (IXAudio2SourceVoice*& v : m_impl->sfx)
        m_impl->xaudio->CreateSourceVoice(&v, &f);
    m_impl->xaudio->CreateSourceVoice(&m_impl->bgmVoice, &f);

    // --- 効果音の生成 ---
    // 発砲: ホワイトノイズ×速い減衰
    {
        const int n = static_cast<int>(kRate * 0.14f);
        m_impl->gunshot.resize(static_cast<size_t>(n));
        uint32_t seed = 0x9E3779B9u;
        for (int i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const float noise = (static_cast<int>((seed >> 9) & 0xFFFF) - 32768) / 32768.0f;
            const float env = expf(-(static_cast<float>(i) / kRate) * 34.0f);
            m_impl->gunshot[static_cast<size_t>(i)] =
                static_cast<int16_t>(noise * env * 30000.0f);
        }
    }
    AddTone(m_impl->hit,   1100.0f, 0.10f, 0.7f, 30.0f);              // 命中: 高いピッ
    AddTone(m_impl->found,  660.0f, 0.09f, 0.6f, 22.0f);             // 発見: 2 音チャイム
    AddTone(m_impl->found,  990.0f, 0.12f, 0.6f, 18.0f);
    AddTone(m_impl->card,   300.0f, 0.05f, 0.5f, 60.0f);             // カード: 短いカチッ

    // --- BGM: 穏やかなアルペジオ（4 音 × 2 小節ぶんを繰り返す想定・低音量ループ）---
    {
        const float notes[8] = { 220.0f, 262.0f, 330.0f, 392.0f,
                                 330.0f, 262.0f, 294.0f, 247.0f };
        for (float f0 : notes) AddTone(m_impl->bgm, f0, 0.42f, 0.28f, 3.2f);
    }

    m_ready = true;
    LOG_INFO("オーディオ初期化（XAudio2 / BGM＋効果音ミックス）");
    return true;
}

void AudioPlayer::Shutdown() {
    if (m_impl) {
        if (m_impl->bgmVoice) { m_impl->bgmVoice->Stop(); m_impl->bgmVoice->DestroyVoice(); }
        for (IXAudio2SourceVoice* v : m_impl->sfx) if (v) { v->Stop(); v->DestroyVoice(); }
        if (m_impl->master) m_impl->master->DestroyVoice();
        if (m_impl->xaudio) m_impl->xaudio->Release();
        delete m_impl; m_impl = nullptr;
    }
    m_ready = false;
}

void AudioPlayer::PlayGunshot() { if (m_impl) m_impl->PlayClip(m_impl->gunshot, 0.7f); }
void AudioPlayer::PlayHit()     { if (m_impl) m_impl->PlayClip(m_impl->hit,     0.6f); }
void AudioPlayer::PlayFound()   { if (m_impl) m_impl->PlayClip(m_impl->found,   0.7f); }
void AudioPlayer::PlayCard()    { if (m_impl) m_impl->PlayClip(m_impl->card,    0.6f); }

void AudioPlayer::PlayBgm() {
    if (m_impl == nullptr || m_impl->bgmVoice == nullptr || m_impl->bgm.empty()) return;
    m_impl->bgmVoice->Stop();
    m_impl->bgmVoice->FlushSourceBuffers();
    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(m_impl->bgm.size() * sizeof(int16_t));
    buf.pAudioData = reinterpret_cast<const BYTE*>(m_impl->bgm.data());
    buf.LoopCount  = XAUDIO2_LOOP_INFINITE;
    m_impl->bgmVoice->SetVolume(0.35f);
    m_impl->bgmVoice->SubmitSourceBuffer(&buf);
    m_impl->bgmVoice->Start(0);
}

void AudioPlayer::StopBgm() {
    if (m_impl && m_impl->bgmVoice) m_impl->bgmVoice->Stop();
}
