#pragma once
// fitom/FnumUtils.h
// F-number テーブル管理ユーティリティ
//
// 旧 CFnumTable クラスをモダナイズ:
//   - グローバルシングルトン (theFnum) を廃止
//   - 各チップドライバが FnumUtils::getTable() で必要なテーブルを取得
//   - マスターピッチは FnumRegistry で一元管理
//   - テーブルはキャッシュされ、同一パラメータで共有される
//
// ─── テーブルサイズ ──────────────────────────────────────────────────────────
//   768 エントリ = 12 半音 × 64 セント/半音 (1セント = 1/100半音)
//   チップドライバは note * 64 + centOffset でインデックスを計算する

#include "fitom/Fnum.h"
#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace fitom {

static constexpr int FNUM_TABLE_SIZE = 768; // 12 半音 × 64 セント

// ================================================================
//  FnumRegistry: F-number テーブルのキャッシュとマスターピッチ管理
// ================================================================
class FnumRegistry {
public:
    static FnumRegistry& instance() {
        static FnumRegistry inst;
        return inst;
    }

    // マスターピッチを設定する (デフォルト 440.0 Hz)
    void setMasterPitch(double pitchHz) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (masterPitch_ != pitchHz) {
            masterPitch_ = pitchHz;
            cache_.clear(); // ピッチ変更でキャッシュ無効化
        }
    }
    double getMasterPitch() const { return masterPitch_; }

    // F-number テーブルを取得 (キャッシュ済みなら再利用)
    // master, divide: チップのマスタークロックと分周比
    // offset: ノートオフセット (OPN: 0, OPM: -61 相当のセント値)
    const uint16_t* getTable(FnumTableType type, int master, int divide, int offset) {
        std::lock_guard<std::mutex> lk(mutex_);
        uint64_t key = makeKey(type, master, divide, offset);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second.data();

        auto& tbl = cache_[key];
        tbl.resize(FNUM_TABLE_SIZE);
        generateTable(type, master, divide, offset, tbl.data());
        return tbl.data();
    }

    // OPM マスターオフセット計算 (旧 GetOPMMasterOffset 相当)
    int16_t getOpmMasterOffset(int sampleRate) const {
        double ret = 768.0 * std::log2(
            (3579545.0 * masterPitch_) / (static_cast<double>(sampleRate) * 440.0));
        return static_cast<int16_t>(std::round(ret));
    }

private:
    FnumRegistry() = default;

    double masterPitch_ = 440.0;
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<uint16_t>> cache_;

    static uint64_t makeKey(FnumTableType type, int master, int divide, int offset) {
        return (static_cast<uint64_t>(type)   << 56)
             | (static_cast<uint64_t>(master)  << 24)
             | (static_cast<uint64_t>(divide)  << 12)
             | (static_cast<uint64_t>(offset + 2048) & 0xFFF);
    }

    void generateTable(FnumTableType type, int master, int divide,
                       int offset, uint16_t* out) {
        for (int i = 0; i < FNUM_TABLE_SIZE; ++i) {
            double freq = masterPitch_ * std::pow(2.0, (offset + i) / 768.0);
            double val  = 0.0;
            switch (type) {
            case FnumTableType::Fnumber:
                // val = freq * 2^17 / master_clock * divide
                val = std::round(freq * (std::pow(2.0, 17.0) / master) * divide);
                break;
            case FnumTableType::TonePeriod:
                // val = 8 * master / freq / divide
                val = std::round((8.0 * master) / (freq * divide));
                break;
            case FnumTableType::DeltaN: {
                // 実チップのDelta-N式: delta_n = round(2^16 * freq * divide / master)
                // (master=マスタークロック[Hz]、divide=ADPCM内部クロック分周比。
                //  YM2608/YM2610は144)。Fnumberケースと同様にdivideを掛ける必要が
                //  あったが、この乗算が丸ごと欠落していた(2026-07-24、ADPCM-B
                //  再生レート異常の実機調査で発覚)。
                //
                //  さらに、ここで使う"freq"の基準周波数は他の型(Fnumber/
                //  TonePeriod)と違いmasterPitch_(A440チューニング基準、
                //  デフォルト440Hz)ではなく、旧FITOM(現在は未使用の
                //  Fnum.cpp::CFnumTable::GetDeltaNが実装していた式)が使っていた
                //  固定16000Hz基準でなければならない。CYmDelta(ADPCM_new.cpp)の
                //  kNoteOffset=448・kPitchOrigin=-57は「旧実装のまま変更しない」
                //  前提でこの16000Hz基準にキャリブレーションされた定数であり、
                //  ここでmasterPitch_(440Hz)を使うと基準周波数が16000/440≈36.4倍
                //  食い違い、DeltaNが常に約1/36というかなり遅い再生レートになって
                //  いた(2026-07-24、C4(MIDIノート60)で実測: 誤り690→正しくは
                //  約25085相当と判明)。旧式のtuningoffset(マスターチューニングを
                //  加算オフセットとして反映)と等価になるよう、
                //  freq*(16000/440)*(masterPitch_/440ではなくmasterPitch_そのもの
                //  が既にfreqに掛かっているため、16000/440倍するだけでよい)。
                constexpr double kDeltaNBaseFreq = 16000.0;
                constexpr double kDeltaNBaseRef  = 440.0; // 旧式のTuningFrequencyデフォルト
                double freqDeltaN = freq * (kDeltaNBaseFreq / kDeltaNBaseRef);
                val = std::round(65536.0 * freqDeltaN * divide / master);
                break;
            }
            case FnumTableType::SSG:
                // SSG period: master / (16 * freq)
                val = std::round(static_cast<double>(master) / (16.0 * freq));
                break;
            default:
                val = 0.0;
                break;
            }
            out[i] = static_cast<uint16_t>(std::clamp(
                static_cast<int>(val), 0, 65535));
        }
    }
};

} // namespace fitom
