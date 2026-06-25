#pragma once
// fitom/PatchManager.h
// パッチ解決・MIDI CHバインディング管理
//
// ─── 役割 ────────────────────────────────────────────────────────────────────
//   PatchManager は以下を担う:
//     1. HwBankRegistry / SwBankRegistry / PatchBank を保持
//     2. ProgChange 時に Patch → ResolvedPatch を構築
//     3. デバイス device_type から適切な HwPatch を選択（チップ族照合）
//     4. バンクファイル (JSON / 旧INI) の読み書き
//
// ─── チップ族照合の仕組み ────────────────────────────────────────────────────
//   Patch::ToneLayer は hwBank/hwProg を持つが、VoiceGroup は指定しない。
//   ProgChange 時に deviceIndex → device_type → VoiceGroup を解決し、
//   HwBankRegistry から対応する HwPatch を取得する。
//
//   照合に失敗した場合 (バンク未登録・プログラム空白):
//     → デフォルト HwPatch (全0) を使用してサイレントに動作継続
//     → FITOM_LOG_WARN を出力

#include "fitom/PatchData.h"
#include "fitom/Log.h"
#include <vector>
#include <memory>
#include <filesystem>
#include <functional>

namespace fitom {

class FITOMConfig;  // デバイスリストへのアクセスに使用

// ================================================================
//  PatchManager
// ================================================================
class PatchManager {
public:
    explicit PatchManager() = default;

    // ─── バンク管理 ───────────────────────────────────────────────

    HwBankRegistry&  hwRegistry() { return hwReg_; }
    SwBankRegistry&  swRegistry() { return swReg_; }

    // パッチバンクの取得・登録
    PatchBank& getPatchBank(int bankNo);
    const PatchBank* findPatchBank(int bankNo) const;

    // ─── プログラムチェンジ解決 ──────────────────────────────────

    // Patch + デバイス情報 → ResolvedPatch を構築する。
    // config はデバイス device_type 解決のために使う。
    ResolvedPatch resolve(const Patch& patch,
                          const FITOMConfig& config) const;

    // 簡易版: bank/prog → Patch → ResolvedPatch
    ResolvedPatch resolve(int patchBank, int prog,
                          const FITOMConfig& config) const;

    // ─── バンクファイル I/O ───────────────────────────────────────

    // JSON 形式でバンクを読み込む
    bool loadHwBankJson(const std::filesystem::path& path,
                        HwBankRegistry::VoiceGroup group, int bankNo);
    bool loadSwBankJson(const std::filesystem::path& path, int bankNo);
    bool loadPatchBankJson(const std::filesystem::path& path, int bankNo);

    // 旧 FITOM INI 形式 (レガシー互換)
    bool loadHwBankLegacy(const std::filesystem::path& path,
                          HwBankRegistry::VoiceGroup group, int bankNo);

    // JSON 形式でバンクを書き出す
    bool saveHwBankJson(const std::filesystem::path& path,
                        HwBankRegistry::VoiceGroup group, int bankNo) const;
    bool saveSwBankJson(const std::filesystem::path& path, int bankNo) const;
    bool savePatchBankJson(const std::filesystem::path& path, int bankNo) const;

    // ─── ドラムバンク ────────────────────────────────────────────
    DrumBankRegistry& drumRegistry() { return drumReg_; }

    const DrumPatch* resolveDrum(int bankNo, int prog) const {
        return drumReg_.resolve(bankNo, prog);
    }

    bool loadDrumBankJson(const std::filesystem::path& path, int bankNo);
    bool saveDrumBankJson(const std::filesystem::path& path, int bankNo) const;
    bool loadDrumBankLegacy(const std::filesystem::path& path, int bankNo);

    // ─── PCM バンク ──────────────────────────────────────────────
    PcmBankRegistry& pcmRegistry() { return pcmReg_; }
    const PcmBankRegistry& pcmRegistry() const { return pcmReg_; }

    // *.pcmbank.json を読み込む
    // adpcm_json フィールドがあれば adpcm_packer の出力 JSON を参照して
    // entries[] を自動構築する
    bool loadPcmBankJson(const std::filesystem::path& path, int bankNo);
    bool savePcmBankJson(const std::filesystem::path& path, int bankNo) const;

    // ─── SCC 波形バンク ──────────────────────────────────────────
    SccWaveRegistry& sccWaveRegistry() { return sccWaveReg_; }

    bool loadSccWaveBankJson(const std::filesystem::path& path, int bankNo);
    bool saveSccWaveBankJson(const std::filesystem::path& path, int bankNo) const;

    // ─── 進捗コールバック (ロード中の GUI 更新用) ────────────────
    using ProgressCallback = std::function<void(const std::string&)>;
    void setProgressCallback(ProgressCallback cb) { progressCb_ = std::move(cb); }

private:
    HwBankRegistry hwReg_;
    SwBankRegistry swReg_;
    DrumBankRegistry drumReg_;
    SccWaveRegistry  sccWaveReg_;
    PcmBankRegistry  pcmReg_;
    std::unordered_map<int, PatchBank> patchBanks_;
    ProgressCallback progressCb_;

    void reportProgress(const std::string& msg) const {
        if (progressCb_) progressCb_(msg);
    }

    // デバイスインデックス → VoiceGroup の解決
    HwBankRegistry::VoiceGroup resolveVoiceGroup(
        int deviceIndex, const FITOMConfig& config) const;
};

// ================================================================
//  PatchResolver: CInstCh 内でインスタンスを持つ軽量ラッパー
//  ProgChange ごとに ResolvedPatch を保持し、NoteOn 時に参照する。
// ================================================================
class PatchResolver {
public:
    PatchResolver() = default;

    // ProgChange 時に呼ぶ
    void apply(const ResolvedPatch& resolved) {
        resolved_ = resolved;
    }

    // NoteOn 時にレイヤーのイテレーションに使う
    int layerCount() const noexcept {
        return resolved_.isValid() ? resolved_.layerCount : 0;
    }

    const ResolvedLayer* layer(int idx) const noexcept {
        if (!resolved_.isValid() || idx < 0 || idx >= resolved_.layerCount)
            return nullptr;
        return &resolved_.layers[idx];
    }

    const SwPatch* swPatch() const noexcept {
        return resolved_.swPatch;
    }

    const Patch* patch() const noexcept {
        return resolved_.patch;
    }

private:
    ResolvedPatch resolved_;
};

} // namespace fitom

// ================================================================
//  DrumBankRegistry をPatchManager に統合
//  (PatchManager.h の末尾に追記)
// ================================================================
#include "fitom/DrumData.h"
#include "fitom/SccWaveData.h"
#include "fitom/PcmBankData.h"
