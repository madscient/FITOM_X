#pragma once
// fitom/SccWaveData.h
// SCC 波形テーブル管理
//
// ─── 設計 ────────────────────────────────────────────────────────────────────
//
//   SCC は各チャンネルに 32 バイトの符号付き波形テーブルを持つ。
//   波形データはチップ族固有のため HwPatch には含めない。
//
//   参照方式:
//     FmHwOp::WS (7bit: 0〜127) → 波形番号 → SccWaveBank から波形を引く
//
//     WS が 0 の場合はデフォルト波形 (矩形波) を使う。
//     同一 WS を複数チャンネルで共有できる (同じ波形番号 → 同じデータ)。
//
//   ファイル形式:
//     *.sccwave.json (SCC Wave Bank)
//     HwBank の chip_group: "PSG" に対応するバンクディレクトリに置く。
//     プロファイルの banks.scc_wave_banks[] で読み込む。
//
//   CSCC::setVoice(ch, hwPatch):
//     hwPatch.hwOp[ch].WS → SccWaveRegistry::getWave(WS) → setWaveform(ch, data)

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <cmath>

namespace fitom {

static constexpr int SCC_WAVE_SIZE     = 32;   // 1 波形のバイト数
static constexpr int SCC_MAX_WAVES     = 128;  // WS の最大値 (FmHwOp::WS は 7bit)

// ================================================================
//  SccWave: 1 波形 (32 バイトの符号付きデータ)
// ================================================================
struct SccWave {
    int8_t  data[SCC_WAVE_SIZE] = {};
    char    name[32]            = {};
    uint8_t waveNo              = 0;   // WS 番号 (0〜127)

    SccWave() noexcept { name[0] = '\0'; }

    // デフォルト: 矩形波
    static SccWave makeSquare() noexcept {
        SccWave w;
        std::snprintf(w.name, sizeof(w.name), "Square");
        for (int i = 0; i < SCC_WAVE_SIZE / 2; ++i) w.data[i] = +100;
        for (int i = SCC_WAVE_SIZE / 2; i < SCC_WAVE_SIZE; ++i) w.data[i] = -100;
        return w;
    }

    // サイン波
    static SccWave makeSine() noexcept {
        SccWave w;
        std::snprintf(w.name, sizeof(w.name), "Sine");
        for (int i = 0; i < SCC_WAVE_SIZE; ++i) {
            double angle = 2.0 * 3.14159265358979 * i / SCC_WAVE_SIZE;
            w.data[i] = static_cast<int8_t>(std::round(100.0 * std::sin(angle)));
        }
        return w;
    }
};

// ================================================================
//  SccWaveBank: WS 番号 → SccWave のバンク
// ================================================================
struct SccWaveBank {
    std::string name;
    std::string filename;

    // WS=0 は常にデフォルト矩形波、WS=1〜127 をユーザー定義
    std::array<SccWave, SCC_MAX_WAVES> waves;

    SccWaveBank() noexcept {
        waves[0] = SccWave::makeSquare();
        waves[1] = SccWave::makeSine();
        for (uint8_t i = 0; i < SCC_MAX_WAVES; ++i) waves[i].waveNo = i;
    }

    const int8_t* getWaveData(uint8_t waveNo) const noexcept {
        return waves[waveNo < SCC_MAX_WAVES ? waveNo : 0].data;
    }
    const SccWave& getWave(uint8_t waveNo) const noexcept {
        return waves[waveNo < SCC_MAX_WAVES ? waveNo : 0];
    }
    void setWave(uint8_t waveNo, const SccWave& w) {
        if (waveNo < SCC_MAX_WAVES) waves[waveNo] = w;
    }
};

// ================================================================
//  SccWaveRegistry: バンク番号 → SccWaveBank のマッピング
//  PatchManager が保持し、CSCC::setVoice から参照される。
// ================================================================
class SccWaveRegistry {
public:
    SccWaveBank& getOrCreate(int bankNo) { return banks_[bankNo]; }

    // WS 番号から波形データを取得 (bankNo=0 がデフォルト)
    const int8_t* getWaveData(int bankNo, uint8_t waveNo) const {
        auto it = banks_.find(bankNo);
        if (it == banks_.end()) {
            // フォールバック: デフォルトバンク (WS=0 = 矩形波)
            return defaultBank_.getWaveData(waveNo);
        }
        return it->second.getWaveData(waveNo);
    }

    const SccWave& getWave(int bankNo, uint8_t waveNo) const {
        auto it = banks_.find(bankNo);
        if (it == banks_.end()) return defaultBank_.getWave(waveNo);
        return it->second.getWave(waveNo);
    }

    bool hasBank(int bankNo) const { return banks_.count(bankNo) > 0; }

private:
    std::unordered_map<int, SccWaveBank> banks_;
    SccWaveBank defaultBank_;   // バンク未登録時のフォールバック
};

} // namespace fitom
