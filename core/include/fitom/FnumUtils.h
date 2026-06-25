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
            case FnumTableType::DeltaN:
                val = std::round(65536.0 * freq / master);
                break;
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
