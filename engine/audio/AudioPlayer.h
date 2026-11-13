#pragma once

#include <cstdint>
#include <vector>

// オーディオ基盤（PROMPT-I-06）。XAudio2 で BGM ループ＋効果音の同時ミックス再生を行う。
// 効果音は手続き生成（音源ファイル不要）。プロセスで 1 つ使うシングルトン。
//
// 使い方: 起動時に AudioPlayer::Get().Init()。各モードで PlayBgm() / 効果音メソッドを呼ぶ。
class AudioPlayer {
public:
    static AudioPlayer& Get();

    bool Init();       // XAudio2 初期化＋効果音/BGM を生成（一度だけ）
    void Shutdown();

    // 効果音（同時再生可能・重ねてミックスされる）
    void PlayGunshot();
    void PlayHit();
    void PlayFound();
    void PlayCard();

    // BGM（ループ）
    void PlayBgm();
    void StopBgm();

private:
    AudioPlayer() = default;
    struct Impl;                 // XAudio2 の詳細は .cpp 側へ隠す
    Impl* m_impl = nullptr;      // 生ポインタ: プロセス寿命のシングルトンで Shutdown で解放
    bool  m_ready = false;
};
