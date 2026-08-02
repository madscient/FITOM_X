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

// SoundFont2ファイル内の1プリセット(phdrレコード)。
struct Sf2Preset {
    std::string name;   // achPresetName (末尾ヌル/空白除去済み、最大20byte)
    uint16_t    bank;   // wBank
    uint16_t    preset; // wPreset (プログラム番号)
};

// SoundFont2(RIFF/sfbk)ファイルの`phdr`(プリセットヘッダ)サブチャンクだけを
// 読み、プリセット名一覧を返す(docs/sf2-fluidsynth-integration.md ⑧節参照)。
// 実際の音声サンプル(LIST/sdta、ファイルサイズの大半を占めうる)はチャンク
// サイズでシークして読み飛ばすため、ファイルが大きくても軽量に処理できる。
// phdr末尾の終端レコード(慣習的に"EOP"、実プリセットではない)は結果に含めない。
// ファイルが開けない・RIFF/sfbk形式でない等、パースできない場合は空配列を
// 返す(例外は投げない。呼び出し元は数値フォールバックする設計のため)。
std::vector<Sf2Preset> parseSf2PresetHeaders(const std::filesystem::path& path);

class Sf2BankRegistry {
public:
    // banks.sf2_banks配列をパースする。file の相対パスは baseDir を起点に
    // 絶対パス化する(他の banks.*[].file と同じ規約、
    // docs/patch-structure-design.md「相対パスの解決基点」参照)。
    // 不正なエントリ(bank/sf2_bankが範囲外、fileが空)は警告して読み飛ばす。
    // 同一bankが複数回指定された場合は後勝ち(警告ログを出す)。
    // 新規に登場したfileごとに一度だけparseSf2PresetHeaders()を呼び、
    // プリセット名解決用テーブル(⑧節)も同じタイミングで構築する。
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

    // プリセット名解決 (⑧節)。CC#32の値(bank)をresolve()と同じ経路で
    // {soundfontIndex, sf2Bank}へ変換した上で、そのSF2ファイルのphdrから
    // (sf2Bank, prog)に一致するachPresetNameを返す。sf2_banksにbankが
    // 無い・該当ファイルのphdrに一致するプリセットが無い、いずれの場合も
    // falseを返す(呼び出し元は数値フォールバックする)。
    bool resolvePresetName(uint8_t cc32Bank, uint8_t prog, std::string& outName) const;

    // resolvePresetName()と同じ引き(phdrルックアップ)を、CC#32の値を経由せず
    // 既に解決済みのsoundfontIndex/sf2Bankから直接行う。MIDIモニター表示用
    // (2026年8月新設): SF2直行パスの窓は`resolve()`結果(soundfontIndex/
    // sf2Bank)をキャッシュしているため、cc32Bankの生値を別途保持しなくても
    // この経路でプリセット名を引ける。soundfontIndexが範囲外、または該当
    // プリセットが無い場合はfalseを返す。
    bool resolvePresetNameByIndex(int soundfontIndex, int sf2Bank, uint8_t prog,
                                   std::string& outName) const;

private:
    struct Entry { int fileIndex; int sf2Bank; };
    std::unordered_map<int, Entry>        byBank_;
    std::vector<std::string>              files_;
    std::unordered_map<std::string, int>  fileIndex_;

    // files_と同じ添字(soundfontIndex)で対応する、(bank<<16|preset)→名前の
    // ルックアップテーブル。load()内、新規fileの初出時に一度だけ構築する。
    std::vector<std::unordered_map<uint32_t, std::string>> presetNamesByFile_;
};

} // namespace fitom
