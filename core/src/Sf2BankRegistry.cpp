// fitom/Sf2BankRegistry.cpp
#include "fitom/Sf2BankRegistry.h"
#include "fitom/Log.h"

#include <fstream>

namespace fitom {

namespace fs = std::filesystem;

// ================================================================
//  SoundFont2 (RIFF/sfbk) phdrパース (docs/sf2-fluidsynth-integration.md ⑧節)
// ================================================================
namespace {

bool readExact(std::ifstream& f, char* buf, std::streamsize n)
{
    f.read(buf, n);
    return static_cast<bool>(f) && f.gcount() == n;
}

bool readU32LE(std::ifstream& f, uint32_t& out)
{
    unsigned char b[4];
    if (!readExact(f, reinterpret_cast<char*>(b), 4)) return false;
    out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
        | (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool readU16LE(std::ifstream& f, uint16_t& out)
{
    unsigned char b[2];
    if (!readExact(f, reinterpret_cast<char*>(b), 2)) return false;
    out = static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8);
    return true;
}

bool readFourCC(std::ifstream& f, char (&out)[4])
{
    return readExact(f, out, 4);
}

bool fourCCEquals(const char (&a)[4], const char* b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// RIFFチャンクは2byte境界にパディングされる(サイズが奇数なら1byte読み飛ばす)。
std::streamoff paddedSize(uint32_t chunkSize)
{
    return static_cast<std::streamoff>(chunkSize) + (chunkSize & 1u);
}

} // namespace

std::vector<Sf2Preset> parseSf2PresetHeaders(const fs::path& path)
{
    std::vector<Sf2Preset> result;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        FITOM_LOG_WARN("SF2 phdr parse: cannot open " << path.string());
        return result;
    }

    char fourcc[4];
    uint32_t riffSize = 0;
    if (!readFourCC(f, fourcc) || !fourCCEquals(fourcc, "RIFF") || !readU32LE(f, riffSize)) {
        FITOM_LOG_WARN("SF2 phdr parse: not a RIFF file: " << path.string());
        return result;
    }
    char form[4];
    if (!readFourCC(f, form) || !fourCCEquals(form, "sfbk")) {
        FITOM_LOG_WARN("SF2 phdr parse: not an sfbk (SoundFont2) file: " << path.string());
        return result;
    }

    // トップレベルは"LIST"チャンク(INFO/sdta/pdta)のみで構成される。
    // "pdta"に到達したら中のサブチャンクをphdrが見つかるまで走査する。
    // 音声サンプル本体を持つ"sdta"はチャンクサイズでシークして読み飛ばす
    // ため、ファイルサイズに関わらず軽量に処理できる。
    while (f) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        if (!readFourCC(f, chunkId) || !readU32LE(f, chunkSize)) break; // EOF: 正常終了

        if (!fourCCEquals(chunkId, "LIST")) {
            // 仕様上トップレベルには通常出現しないが、念のため読み飛ばす。
            f.seekg(paddedSize(chunkSize), std::ios::cur);
            continue;
        }

        char listType[4];
        if (!readFourCC(f, listType)) break;
        const std::streamoff listDataRemaining = static_cast<std::streamoff>(chunkSize) - 4;
        if (listDataRemaining < 0) break; // 不正なサイズ

        if (!fourCCEquals(listType, "pdta")) {
            // INFO/sdta等、pdta以外のLISTは丸ごと読み飛ばす。
            f.seekg(listDataRemaining + (listDataRemaining & 1), std::ios::cur);
            continue;
        }

        std::streamoff remaining = listDataRemaining;
        while (remaining > 0) {
            char subId[4];
            uint32_t subSize = 0;
            if (!readFourCC(f, subId) || !readU32LE(f, subSize)) return result;
            remaining -= 8;

            if (!fourCCEquals(subId, "phdr")) {
                const std::streamoff skip = paddedSize(subSize);
                f.seekg(skip, std::ios::cur);
                remaining -= skip;
                continue;
            }

            // sfPresetHeader: achPresetName[20] + wPreset(2) + wBank(2) +
            // wPresetBagNdx(2) + dwLibrary(4) + dwGenre(4) + dwMorphology(4) = 38byte。
            // 末尾1件は終端センチネル(慣習的に"EOP")のため実プリセットに含めない。
            constexpr size_t kRecordSize = 38;
            const size_t recordCount = subSize / kRecordSize;
            const size_t presetCount = recordCount > 0 ? recordCount - 1 : 0;
            result.reserve(presetCount);

            for (size_t i = 0; i < presetCount; ++i) {
                char nameBuf[20];
                uint16_t wPreset = 0, wBank = 0, wPresetBagNdx = 0;
                uint32_t dwLibrary = 0, dwGenre = 0, dwMorphology = 0;
                if (!readExact(f, nameBuf, sizeof(nameBuf))
                    || !readU16LE(f, wPreset) || !readU16LE(f, wBank)
                    || !readU16LE(f, wPresetBagNdx) || !readU32LE(f, dwLibrary)
                    || !readU32LE(f, dwGenre) || !readU32LE(f, dwMorphology)) {
                    FITOM_LOG_WARN("SF2 phdr parse: truncated phdr record: " << path.string());
                    result.clear();
                    return result;
                }
                Sf2Preset preset;
                preset.name = std::string(nameBuf, sizeof(nameBuf));
                const auto nul = preset.name.find('\0');
                if (nul != std::string::npos) preset.name.erase(nul);
                while (!preset.name.empty() && preset.name.back() == ' ') preset.name.pop_back();
                preset.bank   = wBank;
                preset.preset = wPreset;
                result.push_back(std::move(preset));
            }
            // 終端センチネルレコード分をまとめて読み飛ばす。
            return result;
        }
        return result; // pdtaは見つかったがphdrサブチャンクが無かった(不正な形式)
    }

    FITOM_LOG_WARN("SF2 phdr parse: pdta/phdr chunk not found: " << path.string());
    return result;
}

// ================================================================
//  Sf2BankRegistry
// ================================================================

void Sf2BankRegistry::load(const nlohmann::json& arr, const fs::path& baseDir)
{
    for (const auto& e : arr) {
        int bank        = e.value("bank", -1);
        std::string file = e.value("file", "");
        int sf2Bank     = e.value("sf2_bank", -1);

        if (bank < 0 || bank > 127 || file.empty() || sf2Bank < 0 || sf2Bank > 128) {
            FITOM_LOG_WARN("sf2_banks: invalid entry (bank/file/sf2_bank), skipping");
            continue;
        }

        fs::path path = file;
        if (path.is_relative()) path = baseDir / path;
        std::string absStr = path.lexically_normal().string();

        int fileIdx;
        auto fit = fileIndex_.find(absStr);
        if (fit != fileIndex_.end()) {
            fileIdx = fit->second;
        } else {
            fileIdx = static_cast<int>(files_.size());
            files_.push_back(absStr);
            fileIndex_[absStr] = fileIdx;

            // プリセット名解決用テーブル(⑧節)を、この新規fileの初出時に
            // 一度だけ構築する(soundfont_indexのキャッシュと同じタイミング)。
            std::unordered_map<uint32_t, std::string> names;
            for (const auto& preset : parseSf2PresetHeaders(path)) {
                const uint32_t key = (static_cast<uint32_t>(preset.bank) << 16) | preset.preset;
                names.emplace(key, preset.name);
            }
            presetNamesByFile_.push_back(std::move(names));
        }

        if (byBank_.count(bank)) {
            FITOM_LOG_WARN("sf2_banks: bank=" << bank << " が重複しています。後のエントリで上書きします");
        }
        byBank_[bank] = Entry{fileIdx, sf2Bank};
    }
}

bool Sf2BankRegistry::resolve(uint8_t cc32Bank, Resolved& out) const
{
    auto it = byBank_.find(static_cast<int>(cc32Bank));
    if (it == byBank_.end()) return false;
    out.soundfontIndex = it->second.fileIndex;
    out.sf2Bank        = it->second.sf2Bank;
    return true;
}

bool Sf2BankRegistry::resolvePresetName(uint8_t cc32Bank, uint8_t prog, std::string& outName) const
{
    Resolved r;
    if (!resolve(cc32Bank, r)) return false;
    return resolvePresetNameByIndex(r.soundfontIndex, r.sf2Bank, prog, outName);
}

bool Sf2BankRegistry::resolvePresetNameByIndex(int soundfontIndex, int sf2Bank, uint8_t prog,
                                                std::string& outName) const
{
    if (soundfontIndex < 0
        || static_cast<size_t>(soundfontIndex) >= presetNamesByFile_.size()) {
        return false;
    }
    const auto& names = presetNamesByFile_[soundfontIndex];
    const uint32_t key = (static_cast<uint32_t>(sf2Bank) << 16) | prog;
    auto it = names.find(key);
    if (it == names.end()) return false;
    outName = it->second;
    return true;
}

} // namespace fitom
