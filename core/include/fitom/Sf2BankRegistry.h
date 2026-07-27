#pragma once
// fitom/Sf2BankRegistry.h
// SF2直行パス(docs/sf2-fluidsynth-integration.md参照)における、
// profile の banks.sf2_banks[] を1回だけ解析し、以下の2つの役割を
// 単一クラスから導出するレジストリ。
//   (a) 実行時のCC#32解決用 (CC#32値=bank) → {soundfont_index, sf2_bank}
//   (b) devices[](chip:"SF2")のparams_jsonへ渡す"soundfonts"一覧
//       (重複除去済み・初出順、file→soundfont_indexの逆引きも兼ねる)
//
// PatchManager/HwBankRegistry等とは独立した軽量なレジストリ(SF2の
// presetはHwPatchと型が異なるため、既存レジストリを流用しない設計)。
// FITOMConfig::buildFromProfile() が devices[] のビルドより前に一度だけ
// load() する。

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fitom {

class Sf2BankRegistry {
public:
    // banks.sf2_banks配列をパースする。file の相対パスは baseDir を起点に
    // 絶対パス化する(他の banks.*[].file と同じ規約、
    // docs/patch-structure-design.md「相対パスの解決基点」参照)。
    // 不正なエントリ(bank/sf2_bankが範囲外、fileが空)は警告して読み飛ばす。
    // 同一bankが複数回指定された場合は後勝ち(警告ログを出す)。
    void load(const nlohmann::json& sf2BanksArray, const std::filesystem::path& baseDir);

    bool empty() const { return byBank_.empty(); }

    struct Resolved {
        int soundfontIndex; // soundfontFiles()内のインデックス
        int sf2Bank;        // 0-128 (SF2ファイル内部のネイティブbank番号)
    };

    // CC#32の値(0-127)からエントリを解決する。見つからなければfalseを返す。
    bool resolve(uint8_t cc32Bank, Resolved& out) const;

    // devices[](chip:"SF2")のparams_json["soundfonts"]にそのまま使える、
    // 重複除去済み・初出順の絶対パス一覧。
    const std::vector<std::string>& soundfontFiles() const { return files_; }

private:
    struct Entry { int fileIndex; int sf2Bank; };
    std::unordered_map<int, Entry>        byBank_;
    std::vector<std::string>              files_;
    std::unordered_map<std::string, int>  fileIndex_;
};

} // namespace fitom
