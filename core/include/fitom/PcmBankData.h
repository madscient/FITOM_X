#pragma once
// fitom/PcmBankData.h
// PCM / ADPCM バンクデータ構造
//
// ─── 設計概要 ────────────────────────────────────────────────────────────────
//
//   HwPatch が PCM デバイス (ADPCM / PCMD8) に使われる場合:
//     FmHwOp::WS = PCM バンクのエントリ番号 (0〜127)
//     → SCC と同じ WS 方式で統一
//
//   ワークフロー:
//     [1] adpcm_packer ツール (オフライン)
//           WAV ファイル群 → ADPCM バイナリ (.bin)
//                          → オフセット JSON  (.adpcm.json)
//
//     [2] PCM バンクファイル (*.pcmbank.json)
//           .adpcm.json + .bin のパスを参照
//           エントリ番号 → name / start_offset / end_offset
//
//     [3] 実行時 (CFITOM::initDevices)
//           PcmBankRegistry が .bin を読み込んで CAdPcmBase::loadVoice() に渡す
//
//     [4] NoteOn 時
//           HwPatch.hwOp[0].WS = エントリ番号
//           → CAdPcmBase::updateKey() が voices_[WS] のオフセットを使って発音
//
// ─── adpcm_packer 出力 JSON との対応 ─────────────────────────────────────────
//
//   adpcm_packer の output.json:
//     {
//       "codec": "adpcm-b",
//       "sample_rate": 16000,
//       "entries": [
//         { "name": "se_kick", "offset": 0, "size": 4000, "padded_size": 4096,
//           "offset_hex": "0x000000", "end_hex": "0x000FFF" }
//       ]
//     }
//
//   PcmEntry.startOffset / endOffset は adpcm_packer の
//   offset / (offset + padded_size - 1) をバイト単位で保持する。
//   チップへの書き込みは CAdPcmBase が各チップのアドレス単位に変換する。

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <filesystem>

namespace fitom {

static constexpr int PCM_MAX_ENTRIES = 128; // WS の最大値 (7bit)

// ================================================================
//  PcmEntry: 1 サンプル分のメタ情報 (バイナリ中の位置)
// ================================================================
struct PcmEntry {
    char     name[48]    = {};
    uint32_t startOffset = 0;   // バイナリ内先頭オフセット [byte]
    uint32_t endOffset   = 0;   // バイナリ内末尾オフセット [byte] (パディング込み)
    uint32_t size        = 0;   // ADPCM データサイズ [byte] (パディング前)
    uint32_t paddedSize  = 0;   // バウンダリ整列後サイズ [byte]
    uint8_t  entryNo     = 0;   // WS 番号 (0〜127)
    // 録音時の基準ノート(adpcm_packer出力JSONの"root_note"、省略時69=A4。
    // SampleZone::rootNoteへそのまま渡す用。PatchManager::loadPcmBankJson()
    // がgroup指定時にnamed patchを自動合成する際に使う)。
    uint8_t  rootNote    = 69;

    PcmEntry() noexcept { name[0] = '\0'; }

    bool isValid() const noexcept { return paddedSize > 0; }
};

// ================================================================
//  PcmBank: PCM バンク
//  バイナリデータ + エントリメタ情報を保持する
// ================================================================
struct PcmBank {
    std::string name;
    std::string binPath;        // ADPCM バイナリファイルパス

    // codec / sample_rate は CAdPcmBase に渡す設定値
    std::string codec;          // "adpcm-b" / "adpcm-a" / "pcmd8"
    uint32_t    sampleRate = 0; // ADPCM-B: 8000/16000/24000/32000
    uint32_t    boundary   = 0; // バウンダリ境界 [byte] (32 or 256)

    // このバンクが対象とするVoicePatchType (VOICE_PATCH_ADPCMB/ADPCMA/
    // PCMD8)。profile.jsonのpcm_banks[].groupから設定される
    // (未指定時は0=VOICE_PATCH_NONE)。CFITOM::initDevices()が
    // PCMデバイスへ割り当てるバンク番号をこの値から逆引きするために使う
    // (PcmBankRegistry::findBankNoForVoicePatchType()参照)。また
    // PatchManager::loadPcmBankJson()が、0以外の場合にこのバンクの
    // entries[]から同じ内容のHwBank(named patch)を自動合成する際にも使う。
    uint8_t     voicePatchType = 0;

    // エントリ (WS 番号 → PcmEntry)
    std::array<PcmEntry, PCM_MAX_ENTRIES> entries;

    // バイナリデータ (loadBinary() で読み込む)
    std::vector<uint8_t> binData;

    PcmBank() noexcept {
        entries.fill(PcmEntry{});
    }

    bool hasBinData() const noexcept { return !binData.empty(); }

    const PcmEntry& getEntry(uint8_t entryNo) const noexcept {
        static const PcmEntry null;
        if (entryNo >= PCM_MAX_ENTRIES) return null;
        return entries[entryNo];
    }

    void setEntry(uint8_t entryNo, const PcmEntry& e) {
        if (entryNo < PCM_MAX_ENTRIES) entries[entryNo] = e;
    }

    // バイナリファイルを読み込む
    bool loadBinary(const std::filesystem::path& basePath = {});
};

// ================================================================
//  PcmBankRegistry: バンク番号 → PcmBank のマッピング
// ================================================================
class PcmBankRegistry {
public:
    PcmBank& getOrCreate(int bankNo) { return banks_[bankNo]; }

    const PcmBank* find(int bankNo) const {
        auto it = banks_.find(bankNo);
        return (it != banks_.end()) ? &it->second : nullptr;
    }

    bool hasBank(int bankNo) const { return banks_.count(bankNo) > 0; }

    // voicePatchType (VOICE_PATCH_ADPCMB/ADPCMA/PCMD8) に一致する最初の
    // バンク番号を返す (見つからなければ-1)。CFITOM::initDevices()が
    // 各PCMデバイスに setPcmRegistry() で渡すバンク番号を、そのデバイスの
    // VoicePatchTypeから解決するために使う。
    int findBankNoForVoicePatchType(uint8_t vpt) const {
        if (vpt == 0) return -1;
        for (const auto& kv : banks_) {
            if (kv.second.voicePatchType == vpt) return kv.first;
        }
        return -1;
    }

    // エントリ番号 (WS) から PcmEntry を取得
    const PcmEntry* resolve(int bankNo, uint8_t entryNo) const {
        const PcmBank* b = find(bankNo);
        if (!b) return nullptr;
        const auto& e = b->getEntry(entryNo);
        return e.isValid() ? &e : nullptr;
    }

private:
    std::unordered_map<int, PcmBank> banks_;
};

} // namespace fitom
