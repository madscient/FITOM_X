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

// HWプラグインの契約(IHWPlugin.h の HWPlugin_Render)は、書き込みの反映を描画の先頭から
// 数えて次の64フレーム境界まで遅らせてよいとしている(fluidsynthが64フレーム単位で
// 合成するため)。全プラグインへ同じ境界でイベントを送れば、プラグイン間で揃う。
// タイマーは格子に揃えない。SF2直行パスへの書き込みはMIDIイベントからしか出ないため。
constexpr uint64_t kEventQuantumFrames = 64;

// 曲の先頭から endTimeUs + tailUs までを描画して sink へ渡す。描画したフレーム数を返す。
// イベントは、その時刻(サンプル単位に切り捨て)以降で最初の kEventQuantumFrames の倍数の
// フレームより前に送る。
// タイマーは k ミリ秒目のフレーム(k * sampleRate / 1000 に切り捨て)の直前に k 回目を呼ぶ。
// render が失敗したら std::runtime_error を投げる。
uint64_t renderSong(const SmfSong& song, uint32_t sampleRate, uint64_t tailUs,
                    const OfflineTarget& target, const SampleSink& sink);

} // namespace fitom::render
