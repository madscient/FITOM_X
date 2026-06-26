#ifdef FITOM_HAS_RTAUDIO
// fitom/AudioEngine.cpp
// RtAudio ラッパー実装

#include "fitom/AudioEngine.h"
#include "fitom/Log.h"
#include <stdexcept>
#include <algorithm>

namespace fitom {

// -------------------------------------------------------
//  API 名 → RtAudio::Api
// -------------------------------------------------------
RtAudio::Api AudioEngine::resolveApi(const std::string& apiName)
{
    if (apiName == "wasapi") return RtAudio::WINDOWS_WASAPI;
    if (apiName == "asio")   return RtAudio::WINDOWS_ASIO;
    if (apiName == "ds")     return RtAudio::WINDOWS_DS;
    if (apiName == "alsa")   return RtAudio::LINUX_ALSA;
    if (apiName == "pulse")  return RtAudio::LINUX_PULSE;
    if (apiName == "core")   return RtAudio::MACOSX_CORE;
    return RtAudio::UNSPECIFIED; // auto
}

// -------------------------------------------------------
//  デバイス列挙（GUI・デバッグ用）
// -------------------------------------------------------
std::vector<std::string> AudioEngine::enumerateDevices(const std::string& apiName)
{
    std::vector<std::string> names;
    try {
        RtAudio audio(resolveApi(apiName));
        for (unsigned int id : audio.getDeviceIds()) {
            RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
            if (info.outputChannels >= 2)
                names.push_back(info.name);
        }
    } catch (...) {}
    return names;
}

// -------------------------------------------------------
//  ストリーム開始
// -------------------------------------------------------
bool AudioEngine::start(const AudioConfig&       sysConf,
                        const AudioOutputConfig& profConf,
                        FmEngineRegistry&        registry)
{
    registry_     = &registry;
    bufferFrames_ = sysConf.bufferFrames;

    // RtAudio オブジェクトを再生成（API 変更に対応）
    audio_ = RtAudio(resolveApi(sysConf.api));

    if (audio_.getDeviceCount() == 0) {
        FITOM_LOG_ERR("AudioEngine: no audio devices found");
        return false;
    }

    // デバイス選択（部分一致）
    unsigned int selectedId = audio_.getDefaultOutputDevice();
    if (!profConf.device.empty()) {
        bool found = false;
        for (unsigned int id : audio_.getDeviceIds()) {
            RtAudio::DeviceInfo info = audio_.getDeviceInfo(id);
            if (info.outputChannels >= 2 &&
                info.name.find(profConf.device) != std::string::npos) {
                selectedId = id;
                found = true;
                break;
            }
        }
        if (!found) {
            FITOM_LOG_WARN("AudioEngine: device '" << profConf.device
                << "' not found, using default");
        }
    }

    RtAudio::DeviceInfo info = audio_.getDeviceInfo(selectedId);
    FITOM_LOG_INFO("AudioEngine: device='" << info.name
        << "' sample_rate=" << profConf.sampleRate
        << " buffer_frames=" << bufferFrames_);

    // サンプルレートのサポート確認
    bool rateSupported = std::any_of(
        info.sampleRates.begin(), info.sampleRates.end(),
        [&](unsigned int r){ return r == profConf.sampleRate; });
    if (!rateSupported) {
        FITOM_LOG_WARN("AudioEngine: sample_rate " << profConf.sampleRate
            << " may not be supported by '" << info.name << "'");
    }

    RtAudio::StreamParameters outParams;
    outParams.deviceId     = selectedId;
    outParams.nChannels    = 2;
    outParams.firstChannel = 0;

    unsigned int frames = bufferFrames_;
    RtAudioErrorType err = audio_.openStream(
        &outParams, nullptr,
        RTAUDIO_FLOAT32, profConf.sampleRate,
        &frames,
        &AudioEngine::rtAudioCallback,
        this);

    if (err != RTAUDIO_NO_ERROR) {
        FITOM_LOG_ERR("AudioEngine: openStream failed: " << audio_.getErrorText());
        return false;
    }

    bufferFrames_ = frames; // RtAudio が調整した実際のフレーム数を記録

    err = audio_.startStream();
    if (err != RTAUDIO_NO_ERROR) {
        FITOM_LOG_ERR("AudioEngine: startStream failed: " << audio_.getErrorText());
        audio_.closeStream();
        return false;
    }

    FITOM_LOG_INFO("AudioEngine: stream started (buffer_frames=" << bufferFrames_ << ")");
    return true;
}

// -------------------------------------------------------
//  ストリーム停止
// -------------------------------------------------------
void AudioEngine::stop()
{
    if (audio_.isStreamRunning()) audio_.stopStream();
    if (audio_.isStreamOpen())    audio_.closeStream();
    registry_ = nullptr;
    FITOM_LOG_DEBUG("AudioEngine: stream stopped");
}

bool AudioEngine::isRunning() const
{
    return audio_.isStreamRunning();
}

// -------------------------------------------------------
//  RtAudio コールバック
// -------------------------------------------------------
int AudioEngine::rtAudioCallback(void*              outBuf,
                                 void*              /*inBuf*/,
                                 unsigned int       nFrames,
                                 double             /*streamTime*/,
                                 RtAudioStreamStatus /*status*/,
                                 void*              userData)
{
    auto* self = static_cast<AudioEngine*>(userData);
    auto* out  = static_cast<float*>(outBuf);

    // インターリーブバッファ (L R L R ...) を想定して
    // 非インターリーブの L/R バッファを用意して加算後に変換する
    // スタック割り当てはフレーム数次第なのでスレッドローカル静的バッファを使用
    thread_local std::vector<float> tL, tR;
    tL.resize(nFrames);
    tR.resize(nFrames);

    if (self->registry_) {
        self->registry_->generateAll(tL.data(), tR.data(), nFrames);
    } else {
        std::fill(tL.begin(), tL.end(), 0.0f);
        std::fill(tR.begin(), tR.end(), 0.0f);
    }

    // インターリーブ化
    for (unsigned int i = 0; i < nFrames; ++i) {
        out[i * 2 + 0] = tL[i];
        out[i * 2 + 1] = tR[i];
    }

    return 0;
}

} // namespace fitom

#endif // FITOM_HAS_RTAUDIO
