#pragma once
// apps/fitom_render/OfflineRenderer.h
// SMFのイベントを、描画したサンプル数を時計にしてコアへ流し込む。
// コアの時間は1msタイマーだけで進む(実時間ではタイマースレッドが呼ぶ)ので、
// 1ms分のサンプルを描画するたびにタイマーを1回呼ぶ。

#include "SmfReader.h"

#include <cstdint>
#include <functional>

namespace fitom::render {

struct OfflineTarget {
    std::function<void(const uint8_t*, std::size_t)> sendMidi;   // MIDIバイト列を1イベント分
    std::function<void()>                            tick;       // 1ms タイマー
    std::function<bool(float*, uint32_t)>            render;     // L/R インターリーブ。false = 失敗
};

using SampleSink = std::function<void(const float*, uint32_t)>;

// 曲の先頭から endTimeUs + tailUs までを描画して sink へ渡す。描画したフレーム数を返す。
// イベントは、その時刻(サンプル単位に切り捨て)のフレームより前に送る。
// タイマーは k ミリ秒目のフレーム(k * sampleRate / 1000 に切り捨て)の直前に k 回目を呼ぶ。
// render が失敗したら std::runtime_error を投げる。
uint64_t renderSong(const SmfSong& song, uint32_t sampleRate, uint64_t tailUs,
                    const OfflineTarget& target, const SampleSink& sink);

} // namespace fitom::render
