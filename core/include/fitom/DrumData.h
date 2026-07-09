#pragma once
// fitom/DrumData.h
// ドラムマップデータ構造
//
// ─── 設計方針 ────────────────────────────────────────────────────────────────
//
//   DrumPatch (ドラム用プログラムチェンジ単位):
//     - 128 エントリの DrumNote 配列
//     - 各 DrumNote が「どの Patch で・どのノートを鳴らすか」を保持
//     - Patch は CInstCh と完全に共通 → ToneLayer のマルチレイヤーが有効
//     - SwPatch も Patch が持つものをそのまま使う
//
//   CRhythmCh::noteOn(midiNote, vel):
//     1. DrumNote を引く → patchBank/patchProg で Patch を解決
//     2. PatchResolver::apply(resolvedPatch)
//     3. 各 ToneLayer に対して CInstCh::noteOn と同じ処理
//        - note_range フィルタはスキップ（ドラムは全域で鳴らす）
//        - transpose の代わりに play_note を絶対指定

#include "fitom/PatchData.h"
#include "fitom/FITOMdefine.h"
#include <cstdint>
#include <string>
#include <array>
#include <unordered_map>

namespace fitom {

// ================================================================
//  DrumNote: MIDI ノート番号 1 つ分の発音定義
// ================================================================
struct DrumNote {
    bool    enabled  = false;
    char    name[32] = {};

    // ─── Patch 参照 ───────────────────────────────────────────────
    // CInstCh の CC#0(Bank Select MSB)と全く同じモード選択セマンティクス。
    //   voicePatchType == VOICE_PATCH_NONE(0): 通常モード。
    //     patchBank/patchProgは、CC#0=0時のCC#32/ProgChgと同じ意味
    //     (PatchBank→Patch→ToneLayer[]→HwPatch のマルチレイヤー解決、
    //      PatchManager::resolve(patchBank, patchProg, config)を使う)。
    //   voicePatchType == 0x01-0x6F: 直接モード。
    //     patchBank/patchProgは、CC#0=直接モード時のCC#32/ProgChgと
    //     同じ意味に読み替わる(patchBankはそのVoicePatchType用HwBankの
    //     インデックス、patchProgはそのバンク内のHwProg。
    //     PatchManager::resolveDirect(voicePatchType, patchBank,
    //     patchProg, config, storage)を使う。単層Patch、SwPatch無し)。
    uint8_t voicePatchType = VOICE_PATCH_NONE; // 省略時0=通常モード(後方互換)
    uint8_t patchBank = 0;   // 通常モード:PatchBank番号 / 直接モード:HwBankインデックス
    uint8_t patchProg = 0;   // 通常モード:Progプログラム番号 / 直接モード:HwProg

    // ─── 発音ノート ────────────────────────────────────────────────
    // Patch の ToneLayer.transpose は使わず、ここで絶対指定する
    uint8_t  playNote  = 60;  // 実際に発音する MIDI ノート番号
    int16_t  fineTune  = 0;   // ファインチューニング [cents]

    // ─── チャンネル固定 ────────────────────────────────────────────
    // -1 = 自動割り当て
    //  n = 全 ToneLayer の layer[0] に固定チャンネルを使う
    int8_t   fixedCh   = -1;

    // ─── パフォーマンスパッチ(SwPatch)参照の上書き ─────────────────
    // -1(無指定)の場合、そのノートが参照するHwPatch自身の
    // swBank/swProgにフォールバックする。ビルトインリズム音源
    // (voicePatchType=0x70)はHwPatchがチップ全体で共有される
    // ダミーのため、楽器(fixed_ch)ごとに異なるパフォーマンスパッチを
    // 与えたい場合は、ここで明示的に指定する必要がある。
    // 指定先が解決できなかった場合は、パフォーマンスパッチが
    // 適用されないだけで、無音にはならない(ソフトな失敗)。
    int8_t   swBank    = -1;
    int8_t   swProg    = -1;

    // ─── 発音パラメータ ────────────────────────────────────────────
    int8_t   pan       = 0;    // パンオフセット (-64=L, 0=C, +63=R)
    uint16_t gateTime  = 0;    // ゲートタイム [タイマーティック] (0=NoteOff 待ち)

    DrumNote() noexcept { name[0] = '\0'; }

    bool isActive() const noexcept { return enabled; }
};

// ================================================================
//  DrumPatch: ドラムチャンネルのプログラムチェンジ 1 単位
// ================================================================
struct DrumPatch {
    uint32_t id       = 0xFFFFFFFFu;
    char     name[32] = {};

    std::array<DrumNote, 128> notes;

    DrumPatch() noexcept { name[0] = '\0'; notes.fill(DrumNote{}); }

    bool isValid()     const noexcept { return id != 0xFFFFFFFFu; }

    const DrumNote* getNote(uint8_t midiNote) const noexcept {
        if (midiNote >= 128) return nullptr;
        return notes[midiNote].isActive() ? &notes[midiNote] : nullptr;
    }
};

// ================================================================
//  DrumPatchBank / DrumBankRegistry
// ================================================================
struct DrumPatchBank {
    std::string name;
    std::string filename;
    std::array<DrumPatch, BANK_PROG_SIZE> patches;

    DrumPatchBank() noexcept { patches.fill(DrumPatch{}); }

    const DrumPatch& get(int prog) const noexcept {
        static const DrumPatch null;
        if (prog < 0 || prog >= BANK_PROG_SIZE) return null;
        return patches[prog];
    }
    void set(int prog, const DrumPatch& p) {
        if (prog >= 0 && prog < BANK_PROG_SIZE) patches[prog] = p;
    }
};

class DrumBankRegistry {
public:
    DrumPatchBank& getOrCreate(int bankNo) { return banks_[bankNo]; }

    const DrumPatch* resolve(int bankNo, int prog) const {
        auto it = banks_.find(bankNo);
        if (it == banks_.end()) return nullptr;
        const auto& p = it->second.get(prog);
        return p.isValid() ? &p : nullptr;
    }

    bool hasBank(int bankNo) const { return banks_.count(bankNo) > 0; }

private:
    std::unordered_map<int, DrumPatchBank> banks_;
};

// ================================================================
//  後方互換: 旧 DRUMMAP との変換 (移行ツール用)
// ================================================================
namespace legacy {

struct DRUMMAP {
    void*    device;
    char     name[16];
    uint32_t devID;
    int32_t  ch;
    uint8_t  bank;
    uint8_t  prog;
    int8_t   pan;
    uint8_t  num;
    uint16_t fnum;
    uint16_t gate;
};

// 旧 DRUMMAP → DrumNote
// patchBank/patchProg は呼び出し元が適切に割り当てること
// (旧 bank/prog は HwPatch 参照なので、対応する Patch を作成した上で指定する)
inline DrumNote fromLegacyDrumMap(const DRUMMAP& src,
                                   uint8_t patchBank, uint8_t patchProg) noexcept {
    DrumNote d;
    d.enabled   = true;
    d.patchBank = patchBank;
    d.patchProg = patchProg;
    d.playNote  = src.num;
    d.fineTune  = static_cast<int16_t>(src.fnum);
    d.fixedCh   = static_cast<int8_t>(src.ch);
    d.pan       = src.pan;
    d.gateTime  = src.gate;
    std::strncpy(d.name, src.name, sizeof(d.name) - 1);
    return d;
}

} // namespace legacy

} // namespace fitom
