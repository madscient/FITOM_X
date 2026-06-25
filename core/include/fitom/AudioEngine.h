#pragma once
// fitom/AudioEngine.h
// RtAudio ラッパー — エミュレーターバックエンドの PCM 出力管理
//
// fitom.conf.json の audio.api / audio.buffer_frames と
// プロファイルの audio_output.device / sample_rate を受け取り、
// RtAudio ストリームを開いて FmEngineRegistry::generateAll() を呼ぶ。

#include "fitom/FmEnginePort.h"
#include <RtAudio.h>
#include <string>
#include <functional>
#include <cstdint>

namespace fitom {

struct AudioConfig {
    std::string api;           // "auto" / "wasapi" / "asio" / "alsa" / "pulse" / "core" / "ds"
    uint32_t    bufferFrames;  // fitom.conf.json の audio.buffer_frames
};

struct AudioOutputConfig {
    std::string device;        // profile の audio_output.device (部分一致)
    uint32_t    sampleRate;    // profile の audio_output.sample_rate
};

class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine() { stop(); }

    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ストリームを開いて再生を開始する
    // registry: FmEngineRegistry::generateAll() を呼ぶ
    bool start(const AudioConfig& sysConf,
               const AudioOutputConfig& profileConf,
               FmEngineRegistry& registry);

    void stop();
    bool isRunning() const;

    // 利用可能なデバイス名の一覧を返す（GUI・デバッグ用）
    static std::vector<std::string> enumerateDevices(const std::string& api = "auto");

private:
    static int rtAudioCallback(void* outBuf, void* /*inBuf*/,
                               unsigned int nFrames,
                               double /*streamTime*/,
                               RtAudioStreamStatus /*status*/,
                               void* userData);

    static RtAudio::Api resolveApi(const std::string& apiName);

    RtAudio               audio_;
    FmEngineRegistry*     registry_    = nullptr;
    uint32_t              bufferFrames_ = 512;
};

} // namespace fitom
