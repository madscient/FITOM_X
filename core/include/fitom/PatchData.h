#pragma once
// fitom/PatchData.h
// MIDIパッチ（プログラムチェンジ）データ構造
//
// ─── 設計概念 ────────────────────────────────────────────────────────────────
//
//   Patch (プログラムチェンジ1つ)
//   ├── ToneLayer [0..3]          1チップデバイス = 1レイヤー（最大4つ）
//   │   ├── hw_patch_id           → HwPatch バンクへの参照
//   │   │     HwPatch             チップレジスタに直接書く値 (FmHwVoice + FmHwOp[4])
//   │   └── レイヤー固有設定       音域・トランスポーズ・音量・パン
//   └── sw_patch_id               → SwPatch バンクへの参照（全レイヤー共通）
//         SwPatch                 ソフト処理パラメータ (FmSwVoice + FmSwOp[4])
//
// ─── 設計の意図 ──────────────────────────────────────────────────────────────
//
//   [HwPatch / HwBank]
//     チップ族ごとに独立したバンクで管理する。
//     OPM と OPN はアルゴリズム/レジスタ構造が異なるため、同一パッチに
//     両方の HwPatch を持たせることで「OPM で弾いても OPN で弾いても
//     意図した音色に近い」パッチを作成できる。
//     バンクセレクトは MIDI CC#0/32 で選択するが、チップ族の判定は
//     デバイスの device_type から自動的に行う。
//
//   [SwPatch / SwBank]
//     ソフトLFO・ベロシティ感度はチップ構造に依存しないため、
//     全レイヤーで1つの SwPatch を共有する。
//     チップ族ごとに SwBank を持つ必要はない。
//
//   [ToneLayer]
//     MIDI CHに「OPM の ch1 + DCSG の ch1 を同時発音」という割り当てが
//     できる。各レイヤーは独立したデバイス (profiles の devices[]) を指す。
//     音域制限・トランスポーズで「低音域は DCSG、高音域は OPM」のような
//     スプリット奏法も可能。
//
// ─── バンク番号の扱い ─────────────────────────────────────────────────────────
//
//   MIDI Bank Select:
//     CC#0 (MSB) = HwBank 番号  (0..MAX_HW_BANK-1)
//     CC#32(LSB) = SwBank 番号  (0..MAX_SW_BANK-1)
//   Program Change:
//     プログラム番号 0..127 → Patch を選択
//
//   後方互換:
//     旧来の「デバイス ID でバンク種別を特定」方式は
//     HwBankRegistry::get(device_type, bank_no) が吸収する。

#include "fitom/VoiceData.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <vector>
#include <optional>
#include <unordered_map>

namespace fitom {

// ================================================================
//  定数
// ================================================================
static constexpr int MAX_TONE_LAYERS = 4;    // 1パッチが持てる最大レイヤー数
static constexpr int MAX_HW_BANK     = 128;  // HW バンク数上限
static constexpr int MAX_SW_BANK     = 128;  // SW バンク数上限
static constexpr int BANK_PROG_SIZE  = 128;  // バンク内プログラム数

// HwPatch が持つオペレータ数（FSM音源は4OP固定、OPLLは2OP）
static constexpr int MAX_HW_OPS = 4;

// ================================================================
//  BuiltinRef: OPLL ROM音色(内部生成、opllRomPatches_)への参照。
//  「builtin」フィールド専用バンク(hw_banks[].roleで指定)の各
//  パッチエントリが持つ。通常のops[]とは排他(一方が設定されて
//  いれば、もう一方はJSON上に存在しないことをスキーマで強制する)。
//  2026年7月新設: ROM音色にswBank/swProg(パフォーマンスパッチ)を
//  紐づけるための、prog番号に依存しないuser specificな識別子。
// ================================================================
struct BuiltinRef {
    // 対象チップ。-1=未設定。
    //   0=OPLL / 1=OPLLX / 2=OPLLP / 3=VRC7 (PatchManager::kVariantMap相当)
    //   4=DSG (YM2163、initDsgBuiltinPatches()のビルトイン音色)
    int8_t patchType = -1;
    // ROM/ビルトイン音色番号。-1=未設定。値域はチップごとに異なる:
    //   OPLL系: 1-15 (initOpllRomPatches()のkNames[][]と同じ規約。
    //           0はユーザー音色との衝突回避のため無音として予約)
    //   DSG:    0-19 (initDsgBuiltinPatches()、prog 0 も正規の音色)
    int8_t patchNo = -1;

    // 未設定のセンチネルは0ではなく-1であることに注意。DSGはprog 0
    // (St.Percussive)が正規の音色のため、patchNo>=1を要求すると
    // そのエントリだけが黙って一致しなくなる。
    bool isValid() const noexcept { return patchType >= 0 && patchNo >= 0; }
};

// BuiltinRef::patchType の値 (JSONの "builtin.patch_type" 文字列と対応)。
// PatchManager.cpp の kBuiltinTypeNames / kBuiltinTypeMap が唯一の
// 文字列変換元で、この定数はコード側から参照するための別名。
enum : int8_t {
    BUILTIN_TYPE_OPLL  = 0,
    BUILTIN_TYPE_OPLLX = 1,
    BUILTIN_TYPE_OPLLP = 2,
    BUILTIN_TYPE_VRC7  = 3,
    BUILTIN_TYPE_DSG   = 4,
    BUILTIN_TYPE_COUNT = 5,
};

// ================================================================
//  HwPatch: ハードウェアレジスタイメージ
//  チップに直接書き込む値のみを保持する。
//  ソフトパラメータ・ベロシティ感度は含まない。
// ================================================================
struct HwPatch {
    uint32_t   id;            // バンク内 ID (bank_no << 16 | prog_no)
    char       name[32];      // パッチ名 (UTF-8)

    FmHwVoice  hw;            // チャンネル HW パラメータ
    FmHwOp     hwOp[MAX_HW_OPS]; // オペレータ HW パラメータ
    FmChipExt  ext;           // チップ固有拡張 (OPZ 等)

    // ROM/ビルトイン音色への参照(builtin専用バンクのみ使用、通常のops[]と
    // 排他)。isValid()がtrueの場合、hw/hwOp[]/extの内容は無視される
    // (ROM音色自体のFMパラメータはopllRomPatches_/dsgBuiltinPatches_由来の
    // まま変更できないため)。
    BuiltinRef builtin;

    // ─── パフォーマンスパッチ(SwPatch)参照 ────────────────────────
    // -1 = 参照なし(センチネル)。このHwPatch(音色データ)自身が
    // 「本来意図している演奏特性」のデフォルトを指す。DrumNote側に
    // 個別の上書き指定(swBank/swProg != -1)があれば、そちらが優先
    // される (詳細は docs/terminology.md 参照)。
    // 指定先が解決できなかった場合は、パフォーマンスパッチが
    // 適用されないだけで、HwPatch自体の発音は妨げられない
    // (ソフトな失敗、エラー扱いにしない)。
    int8_t     swBank = -1;
    int8_t     swProg = -1;

    HwPatch() noexcept : id(0xFFFFFFFFu) { name[0] = '\0'; }

    bool isValid() const noexcept { return id != 0xFFFFFFFFu; }

    // 旧 FMVOICE から変換
    static HwPatch fromLegacy(const legacy::FMVOICE& src, uint32_t bank, uint32_t prog) noexcept {
        FmVoice fv = legacy::fromLegacy(src);
        HwPatch p;
        p.id   = (bank << 16) | prog;
        std::strncpy(p.name, fv.name, sizeof(p.name) - 1);
        p.hw   = fv.hw;
        for (int i = 0; i < MAX_HW_OPS; ++i) p.hwOp[i] = fv.hwOp[i];
        p.ext  = fv.ext;
        return p;
    }

    // 旧 FMVOICE へ変換 (GUI後方互換)
    legacy::FMVOICE toLegacy() const noexcept {
        FmVoice fv;
        fv.id = id;
        std::strncpy(fv.name, name, sizeof(fv.name) - 1);
        fv.hw  = hw;
        for (int i = 0; i < MAX_HW_OPS; ++i) fv.hwOp[i] = hwOp[i];
        fv.ext = ext;
        return legacy::toLegacy(fv);
    }
};

// ================================================================
//  SwPatch: ソフトウェア処理パラメータ
//  チップ族に依存しない。全レイヤーで共有される。
// ================================================================
struct SwPatch {
    uint32_t   id;            // バンク内 ID
    char       name[32];      // パッチ名 (UTF-8)

    FmSwVoice  sw;            // チャンネル LFO (ビブラート)
    FmSwOp     swOp[MAX_HW_OPS]; // オペレータ単位 (トレモロ / ベロシティ感度)

    // 固定トランスポーズ [セント、-1200〜+1200]。ToneLayer.transpose
    // (半音単位)とは独立 (ToneLayer.transposeはレイヤードパッチの
    // レイヤー固有パラメータ、こちらはSwPatch=演奏特性の一部として、
    // 直接モード等でも効かせられる)。両方が有効な場合は加算する。
    // 適用時は「半音部分(ノート番号に加算)」と「セント端数部分
    // (kfs単位に変換して加算。docs/terminology.mdの「kfs」参照)」に
    // 分解される (MidiCh.cpp参照)。名前は DrumNote::fineTune と同じ
    // 命名スタイル (ToneLayer.transposeとの役割混同を避けるため
    // "fixed"ではなく"fine"を使う、2026年7月)。
    // 固定ディレイは2026年7月時点で保留(別途設計を要する)。
    int16_t    fineTranspose = 0;

    SwPatch() noexcept : id(0xFFFFFFFFu) { name[0] = '\0'; }
    bool isValid() const noexcept { return id != 0xFFFFFFFFu; }
};

// ================================================================
//  HwBank: HwPatch のバンク (チップ族ごとに独立)
// ================================================================
struct HwBank {
    std::string name;                              // バンク名
    std::string filename;                          // ソースファイルパス
    std::array<HwPatch, BANK_PROG_SIZE> patches;  // 128エントリ

    // このバンク全体で共通の VoicePatchType (VOICE_PATCH_*)。
    // 「同一HwBank内は同一VoicePatchTypeのみ」という規則の下、
    // バンク単位で1つだけ保持する (パッチ単位では持たない)。
    // 0 = 未設定 (旧データ・後方互換用)。
    uint8_t voicePatchType = 0;

    HwBank() { for (auto& p : patches) p = HwPatch{}; }

    const HwPatch& get(int prog) const noexcept {
        static const HwPatch null;
        if (prog < 0 || prog >= BANK_PROG_SIZE) return null;
        return patches[prog];
    }
    void set(int prog, const HwPatch& p) {
        if (prog >= 0 && prog < BANK_PROG_SIZE) patches[prog] = p;
    }

    // builtin専用バンク(hw_banks[].role=="builtin_swpatch_meta")内を、
    // (patchType, patchNo)の一致で線形探索する。
    // 通常のprog番号による機械的対応ではなく、パッチ設計者が明示的に
    // 指定したbuiltin参照でマッチングするため、一致するエントリが
    // バンク内のどのprog番号にあっても構わない。patchTypeも一致条件に
    // 含まれるため、1つのメタバンクファイルに複数チップ(OPLL系/DSG)の
    // エントリを混在させてよい。
    const HwPatch* findByBuiltinRef(int8_t patchType, int8_t patchNo) const noexcept {
        for (const auto& p : patches) {
            if (p.isValid() && p.builtin.patchType == patchType
                && p.builtin.patchNo == patchNo) {
                return &p;
            }
        }
        return nullptr;
    }
};

// ================================================================
//  SwBank: SwPatch のバンク (チップ族によらず共通)
// ================================================================
struct SwBank {
    std::string name;
    std::string filename;
    std::array<SwPatch, BANK_PROG_SIZE> patches;

    SwBank() { for (auto& p : patches) p = SwPatch{}; }

    const SwPatch& get(int prog) const noexcept {
        static const SwPatch null;
        if (prog < 0 || prog >= BANK_PROG_SIZE) return null;
        return patches[prog];
    }
    void set(int prog, const SwPatch& p) {
        if (prog >= 0 && prog < BANK_PROG_SIZE) patches[prog] = p;
    }
};

// ================================================================
//  HwBankRegistry: (voicePatchType, バンク番号) → HwBank のマッピング
//
//  旧 CFITOMConfig の vOpmBank[] / vOpnBank[] 等を統合する。
//  【重要】ここをVoiceGroupキーにしてはならない (SampleZoneBankRegistryと
//  同じ理由)。VoiceGroupはOPM/OPZ/OPZ2、OPL/OPL2/OPL3_2、OPLL/OPLLP/OPLLX/
//  VRC7をそれぞれ1つに束ねるため、VoiceGroupキーだと同じバンク番号を族内の
//  別チップへ割り当てられず、プロファイルのhw_banks[]に同一バンク番号で
//  チップ違いのエントリを並べても後から読み込んだ側が前の登録を上書きして
//  しまう。バンク番号の名前空間はチップ (voicePatchType) ごとに独立させ、
//  「どのチップのバンクか」はプロファイルのhw_banks[].groupが決める。
// ================================================================
class HwBankRegistry {
public:
    // バンクを登録・取得
    HwBank& getOrCreate(uint8_t voicePatchType, int bankNo) {
        return banks_[voicePatchType][bankNo];
    }
    const HwBank* find(uint8_t voicePatchType, int bankNo) const {
        auto it = banks_.find(voicePatchType);
        if (it == banks_.end()) return nullptr;
        auto it2 = it->second.find(bankNo);
        if (it2 == it->second.end()) return nullptr;
        return &it2->second;
    }
    // SysExによるプリセットバンク直接編集(target-type=0x01)用の
    // 可変アクセサ(2026年7月新設)。
    HwBank* findMutable(uint8_t voicePatchType, int bankNo) {
        auto it = banks_.find(voicePatchType);
        if (it == banks_.end()) return nullptr;
        auto it2 = it->second.find(bankNo);
        if (it2 == it->second.end()) return nullptr;
        return &it2->second;
    }

    // 指定voicePatchTypeに登録済みのバンク番号一覧を昇順で返す(GUIの
    // パッチピッカーダイアログ向け、直接デバイス選択モードのCC#32階層
    // 列挙用、2026年7月新設)。該当チップが未登録なら空を返す。
    std::vector<int> listBankNumbers(uint8_t voicePatchType) const {
        std::vector<int> result;
        auto it = banks_.find(voicePatchType);
        if (it == banks_.end()) return result;
        result.reserve(it->second.size());
        for (const auto& kv : it->second) result.push_back(kv.first);
        std::sort(result.begin(), result.end());
        return result;
    }

    // 何らかのバンクが登録されているvoicePatchTypeの一覧を昇順で返す
    // (「要求されたチップにバンクが無いが、同じ族の別チップにはある」
    // 状況を診断するために使う)。
    std::vector<uint8_t> listVoicePatchTypes() const {
        std::vector<uint8_t> result;
        result.reserve(banks_.size());
        for (const auto& kv : banks_) result.push_back(kv.first);
        std::sort(result.begin(), result.end());
        return result;
    }

    // HwPatch の解決:
    //   指定の voicePatchType/bank/prog から HwPatch を返す。
    //   見つからない場合は nullptr。
    const HwPatch* resolve(uint8_t voicePatchType, int bankNo, int prog) const {
        const HwBank* b = find(voicePatchType, bankNo);
        if (!b) return nullptr;
        const auto& p = b->get(prog);
        return p.isValid() ? &p : nullptr;
    }

private:
    std::unordered_map<uint8_t,
        std::unordered_map<int, HwBank>> banks_;
};

// ================================================================
//  サンプルベース音源 (波形メモリ/PCM) 系共通ボイスパッチ
//  VOICE_PATCH_AWM (YMF278 AWM部+YRW801 ROM等) で使用。
//  ADPCM系 (ADPCM-A/ADPCM-B/PCMD8等) も将来的にこのスキーマを転用する
//  ことを想定した、チップ非依存の汎用設計としている。
//
//  設計意図: サンプルベース音源は「1プログラム = 複数キーゾーン
//  (+ベロシティレイヤー) への波形/サンプルマッピング」という、
//  FMオペレータ型のHwPatch(AR/DR/SL/RR等を持つ)とは本質的に異なる
//  形状のデータを持つ。無理にHwPatch/FmChipExtへ押し込めると
//  (a) FmChipExtが想定する「少数の追加スカラー値」という設計から外れる
//  (b) 使われないAR/DR/SL/RR等のフィールドが無意味に残る
//  ため、HwPatch/HwBank/HwBankRegistryとは完全に独立した専用の型を
//  新設する。既存チップ(OPN/OPM/OPL3等)のHwPatch関連コードには
//  一切影響を与えない。
//
//  上位層 (Patch/ToneLayer が hw_bank/hw_prog をインデックスとして
//  参照する構造) は変更不要。voicePatchType がサンプルベース系
//  (VOICE_PATCH_AWM等) の場合のみ、PatchManager::resolve() が
//  HwBankRegistryではなくこちらを検索し、
//  ResolvedLayer::samplePatch に結果を格納する。
//
//  waveIndex の意味はチップ依存の「生値」であり、このスキーマ自体は
//  解釈しない (OPL4AWMなら内蔵ROMの波形番号、ADPCM系ならPcmBankの
//  エントリ番号(WS値)、というように、各チップドライバのresolveXxx()が
//  解釈する)。
// ================================================================

// 1ゾーン: このノート範囲・ベロシティ範囲では指定の波形/サンプルを使う。
// (現状のOPL4AWM実装のkAllRegions[]相当の1エントリに対応)
struct SampleZone {
    uint8_t  keyMin = 0;
    uint8_t  keyMax = 127;
    // ベロシティレイヤー (ベロシティスイッチ)。0-127/0-127 (無制限) が既定値
    // のため、ベロシティレイヤーを使わないパッチ(OPL4AWM現行実装等)は
    // このフィールドを省略しても後方互換に動作する。
    uint8_t  velMin = 0;
    uint8_t  velMax = 127;
    uint16_t waveIndex = 0;   // チップ側のROM/PCM波形番号 (チップ依存の生値)
    // 録音時の基準ノート (MIDI note番号)。ピッチ可変のPCM系チップ
    // (ADPCM-B/PCMD8等)がノートからの相対ピッチシフト量を計算するのに
    // 使う。OPL4AWMは下記pitchOffset/keyScalingベースの計算(ALSA
    // sound/drivers/opl4/yrw801.cと同一規約)を使うため未使用
    // (0のままでよい)。将来のADPCM系転用に備えた予約。
    //
    // 【重要・ADPCM拡張時の必須知識】旧FITOMでは、ADPCM-Bのサンプルは
    // 「A4=440Hz(MIDI note 69)でサンプリングされている」という暗黙の
    // 制約でコーディングされている。ADPCM-B/PCMD8等にこのスキーマを
    // 転用する際、既存のサンプル資産と後方互換を保つには、rootNoteの
    // 既定値は69(A4)にすべきであり、60(C4)にしてはならない。
    // (このコメントは会話の文脈が失われても参照できるよう、意図的に
    //  ここに残している)
    uint8_t  rootNote = 69;  // 既定: A4 (MIDI note 69)。旧FITOM ADPCM-B
                              // の暗黙の基準ピッチ規約に合わせている。

    // ─── OPL4AWM専用: 波形ごとのピッチ/音量校正値 ─────────────────
    // YRW801等のROM波形は、絶対Hz→Fnumber/Octaveの汎用計算式だけでは
    // 正しい音高・音量にならない(ROM波形自体がどのピッチ・音量で
    // 収録されているかは実測でしか分からないため)。ALSAドライバ
    // (sound/drivers/opl4/yrw801.c、opl4_sound構造体)がYRW801実機向けに
    // 実測・埋め込んでいる値と同一規約でここに持ち、COPL4AWM
    // (core/src/OPL4.cpp)がgetFnumber()/updateVolExp()で使う
    // (2026年8月新設。ユーザー報告『AWMが意図した波形と違う音に聞こえる』
    // の調査で、この校正値が無いと波形によっては数オクターブ単位で
    // ピッチがずれることが判明したため、ALSAの値を丸ごと移植した)。
    // OPL4AWM以外のチップでは未使用(既定値のままでよい)。
    int16_t  pitchOffset   = 0;    // 100/128セント単位
    uint8_t  keyScaling    = 100;  // % (100=通常のノート追従、0=固定ピッチ)
    uint8_t  toneAttenuate = 0;    // 加算減衰量(7bit)
    uint8_t  volumeFactor  = 254;  // 音量スケール(0-254、254=無補正)

    // ─── パフォーマンスパッチ(SwPatch)参照 ────────────────────────
    // HwPatch::swBank/swProgと同じ意味・同じソフトフォールバック規約
    // (-1=参照なし、指定先が解決できなくてもゾーン自体の発音は妨げない)。
    // 2026年7月新設。DrumNote側に個別の上書き指定があれば、そちらが優先
    // される(詳細はdocs/patch-structure-design.md参照)。
    // 【対応チップの制約】ADPCM-B/PCMD8は全機能(チャンネルLFO/トレモロ/
    // VTL感度/fine_transpose)が有効。ADPCM-Aはピッチ制御が無いため
    // VTL感度/トレモロのみ有効(チャンネルLFO/fine_transposeは無効)。
    // AWMは実機のLFO/VIBレジスタ・波形バイナリ側の設定と整合させる設計が
    // 別途必要なため、現状この参照は解決されるが音には反映されない
    // (2026年7月時点、docs/patch-structure-design.md参照)。
    int8_t   swBank    = -1;
    int8_t   swProg    = -1;
};

// 1プログラムぶんのゾーン列。ノート・ベロシティに応じて↑を線形探索し、
// 該当するSampleZone::waveIndexを使う (現状のresolveWaveNumber()相当)。
// zonesは可変長 (プログラムによってゾーン数が異なるため、HwPatchのような
// 固定長オペレータ配列では表現できない)。
struct SampleZonePatch {
    uint32_t id;              // バンク内 ID (bank_no << 16 | prog_no)、HwPatchと同じ規約
    char     name[32];        // パッチ名 (UTF-8)
    std::vector<SampleZone> zones;

    SampleZonePatch() noexcept : id(0xFFFFFFFFu) { name[0] = '\0'; }

    bool isValid() const noexcept { return id != 0xFFFFFFFFu; }

    // ノート・ベロシティに一致する最初のゾーンを返す(該当なしなら
    // zones[0]にフォールバック、zones自体が空ならnullptr)。COPL4AWM/
    // CAdPcmBaseに重複していた線形探索を統一する正準実装(2026年7月)。
    const SampleZone* resolveZone(uint8_t note, uint8_t vel) const noexcept {
        for (const auto& z : zones) {
            if (note >= z.keyMin && note <= z.keyMax &&
                vel  >= z.velMin && vel  <= z.velMax) {
                return &z;
            }
        }
        return zones.empty() ? nullptr : &zones[0];
    }
};

// ================================================================
//  SampleZoneBank: SampleZonePatch のバンク (HwBankと対になる構造)
// ================================================================
struct SampleZoneBank {
    std::string name;
    std::string filename;
    std::array<SampleZonePatch, BANK_PROG_SIZE> patches;

    // HwBank::voicePatchType と同じ意図。このバンクを使うチップの
    // VoicePatchType (VOICE_PATCH_AWM、将来的にADPCM系の値等) を保持する。
    uint8_t voicePatchType = 0;

    SampleZoneBank() { for (auto& p : patches) p = SampleZonePatch{}; }

    const SampleZonePatch& get(int prog) const noexcept {
        static const SampleZonePatch null;
        if (prog < 0 || prog >= BANK_PROG_SIZE) return null;
        return patches[prog];
    }
    void set(int prog, const SampleZonePatch& p) {
        if (prog >= 0 && prog < BANK_PROG_SIZE) patches[prog] = p;
    }
};

// ================================================================
//  SampleZoneBankRegistry: (voicePatchType, バンク番号) → SampleZoneBank
//  のマッピング。HwBankRegistryのVoiceGroupキーと同じ役割をvoicePatchType
//  で担う(2026年7月修正)。
//  【重要】ここをVoiceGroupキーにしてはならない。
//  FITOMConfig::voicePatchTypeToVoiceGroup()はADPCM-B/ADPCM-A/PCMD8/AWMを
//  全てVOICE_GROUP_PCMへ束ねるため、VoiceGroupキーだと逆にこの4チップ族が
//  バンク番号空間を共有してしまい、本来分離したい対象が分離できない。
//  (以前はバンク番号のみをキーにしたフラットな空間だったが、AWMバンク0/1と
//  ADPCM-B/ADPCM-Aのバンク0/1が番号衝突し、後から読み込んだ側が前の登録を
//  上書きしてパッチピッカーからAWMバンクが消える不具合があった)。
// ================================================================
class SampleZoneBankRegistry {
public:
    SampleZoneBank& getOrCreate(uint8_t voicePatchType, int bankNo) {
        return banks_[voicePatchType][bankNo];
    }

    const SampleZoneBank* find(uint8_t voicePatchType, int bankNo) const {
        auto it = banks_.find(voicePatchType);
        if (it == banks_.end()) return nullptr;
        auto it2 = it->second.find(bankNo);
        return (it2 != it->second.end()) ? &it2->second : nullptr;
    }
    bool hasBank(uint8_t voicePatchType, int bankNo) const {
        auto it = banks_.find(voicePatchType);
        if (it == banks_.end()) return false;
        return it->second.count(bankNo) > 0;
    }

    // 指定voicePatchTypeに登録済みのバンク番号一覧を昇順で返す(GUIの
    // パッチピッカーダイアログ向け、サンプルベース音源系(ADPCM-B/
    // ADPCM-A/PCMD8/AWM)のCC#32階層列挙用、2026年7月新設)。
    std::vector<int> listBankNumbers(uint8_t voicePatchType) const {
        std::vector<int> result;
        auto it = banks_.find(voicePatchType);
        if (it == banks_.end()) return result;
        result.reserve(it->second.size());
        for (const auto& kv : it->second) result.push_back(kv.first);
        std::sort(result.begin(), result.end());
        return result;
    }

    const SampleZonePatch* resolve(uint8_t voicePatchType, int bankNo, int prog) const {
        const SampleZoneBank* b = find(voicePatchType, bankNo);
        if (!b) return nullptr;
        const auto& p = b->get(prog);
        return p.isValid() ? &p : nullptr;
    }

private:
    std::unordered_map<uint8_t, std::unordered_map<int, SampleZoneBank>> banks_;
};

// ================================================================
//  SwBankRegistry: バンク番号 → SwBank のマッピング
// ================================================================
class SwBankRegistry {
public:
    SwBank& getOrCreate(int bankNo) { return banks_[bankNo]; }

    // const アクセサ
    const SwBank* find(int bankNo) const {
        auto it = banks_.find(bankNo);
        return (it != banks_.end()) ? &it->second : nullptr;
    }
    // SysExによるプリセットバンク直接編集(target-type=0x01)用の
    // 可変アクセサ(2026年7月新設)。
    SwBank* findMutable(int bankNo) {
        auto it = banks_.find(bankNo);
        return (it != banks_.end()) ? &it->second : nullptr;
    }
    bool hasBank(int bankNo) const { return banks_.count(bankNo) > 0; }

    const SwPatch* resolve(int bankNo, int prog) const {
        const SwBank* b = find(bankNo);
        if (!b) return nullptr;
        const auto& p = b->get(prog);
        return p.isValid() ? &p : nullptr;
    }

private:
    std::unordered_map<int, SwBank> banks_;
};

// ================================================================
//  ToneLayer: 1チップデバイス分の発音レイヤー
// ================================================================
struct ToneLayer {
    // ─── デバイス参照 ─────────────────────────────────────────────
    // voicePatchType (VOICE_PATCH_*): バンクセレクトLSB直接指定モードと
    // 同じ方式でデバイスを選択する。実際のデバイスは
    // Config::findDeviceIndexByVoicePatchType() で実行時に解決する。
    // 0 (VOICE_PATCH_NONE) = 無効 (設定禁止の予約値)。
    uint8_t voicePatchType;

    // ─── HW パッチ参照 ────────────────────────────────────────────
    // hw_bank / hw_prog はチップ族から適切な HwBank を引くためのキー。
    // 実際の VoiceGroup は voicePatchType から一意に決定する。
    // 注意: これらは Patch プリセット (JSON) に静的に埋め込まれた値であり、
    // ライブの MIDI Bank Select (CC#0/CC#32) からは供給されない。
    // どのデバイス・どの HW バンクで鳴らすかは Patch 定義時に固定される。
    uint8_t hwBank;       // HW バンク番号
    uint8_t hwProg;       // HW プログラム番号

    // ─── レイヤー固有 MIDI パラメータ ────────────────────────────
    uint8_t  noteRangeLo;   // 発音する最低音 (MIDIノート番号, 0=無制限)
    uint8_t  noteRangeHi;   // 発音する最高音 (127=無制限)
    int8_t   transpose;     // トランスポーズ [半音] (-48..+48)
    int8_t   volumeOffset;  // 音量オフセット [dB相当, -64..+63]
    int8_t   panOffset;     // パンオフセット (-64=L, 0=C, +63=R)
    bool     enabled;       // このレイヤーを使用するか

    ToneLayer() noexcept
        : voicePatchType(0)
        , hwBank(0), hwProg(0)
        , noteRangeLo(0), noteRangeHi(127)
        , transpose(0), volumeOffset(0), panOffset(0)
        , enabled(false) {}

    bool isActive() const noexcept {
        return enabled && voicePatchType != 0;
    }

    // ノートがこのレイヤーの音域に含まれるか
    bool inRange(uint8_t note) const noexcept {
        return note >= noteRangeLo && note <= noteRangeHi;
    }

    // トランスポーズ適用後のノート番号
    int transposedNote(uint8_t note) const noexcept {
        return static_cast<int>(note) + transpose;
    }
};

// ================================================================
//  Patch: MIDIプログラムチェンジ1つに対応するデータ
// ================================================================
struct Patch {
    uint32_t id;          // バンク内 ID
    char     name[32];    // パッチ名 (UTF-8)

    // ─── ToneLayer ────────────────────────────────────────────────
    // layer[0] がプライマリ。layer[1..3] は省略可能（enabled=false）。
    std::array<ToneLayer, MAX_TONE_LAYERS> layers;

    // ─── パッチ共通パラメータ ─────────────────────────────────────
    uint8_t poly;     // ポリフォニー数 (0=チャンネルマップのデフォルト)

    Patch() noexcept
        : id(0xFFFFFFFFu), poly(0)
    {
        name[0] = '\0';
    }

    bool isValid() const noexcept { return id != 0xFFFFFFFFu; }

    // アクティブなレイヤー数を返す
    int activeLayerCount() const noexcept {
        int n = 0;
        for (const auto& l : layers) if (l.isActive()) ++n;
        return n;
    }

    // 後方互換: 旧 FMVOICE 1本からシングルレイヤーパッチを作成
    // 直接モード (バンクセレクトLSB) の単層パッチ構築にも使用する。
    static Patch fromSingleLayer(const HwPatch& hwp, uint8_t voicePatchType,
                                  uint32_t bank, uint32_t prog) noexcept {
        Patch p;
        p.id = (bank << 16) | prog;
        std::strncpy(p.name, hwp.name, sizeof(p.name) - 1);
        p.layers[0].voicePatchType = voicePatchType;
        p.layers[0].hwBank      = static_cast<uint8_t>(bank & 0xFF);
        p.layers[0].hwProg      = static_cast<uint8_t>(prog & 0x7F);
        p.layers[0].enabled     = true;
        return p;
    }

    // サンプルベース音源系 (VOICE_PATCH_AWM等) 向けオーバーロード。
    // ToneLayerが持つのは voicePatchType/hwBank/hwProg というインデックス
    // 参照のみであり、実際のデータ形状(HwPatchかSampleZonePatchか)には
    // 依存しないため、パッチ名のコピー元が違うだけで構造は全く同じ。
    static Patch fromSingleLayer(const SampleZonePatch& szp, uint8_t voicePatchType,
                                  uint32_t bank, uint32_t prog) noexcept {
        Patch p;
        p.id = (bank << 16) | prog;
        std::strncpy(p.name, szp.name, sizeof(p.name) - 1);
        p.layers[0].voicePatchType = voicePatchType;
        p.layers[0].hwBank      = static_cast<uint8_t>(bank & 0xFF);
        p.layers[0].hwProg      = static_cast<uint8_t>(prog & 0x7F);
        p.layers[0].enabled     = true;
        return p;
    }
};

// ================================================================
//  PatchBank: Patch のバンク
// ================================================================
struct PatchBank {
    std::string name;
    std::string filename;
    std::array<Patch, BANK_PROG_SIZE> patches;

    PatchBank() { for (auto& p : patches) p = Patch{}; }

    const Patch& get(int prog) const noexcept {
        static const Patch null;
        if (prog < 0 || prog >= BANK_PROG_SIZE) return null;
        return patches[prog];
    }
    void set(int prog, const Patch& p) {
        if (prog >= 0 && prog < BANK_PROG_SIZE) patches[prog] = p;
    }
};

// ================================================================
//  ResolvedPatch: プログラムチェンジ処理時に解決済みの参照をまとめた構造体
//  CInstCh::ProgChange が呼ばれるたびに構築する。
// ================================================================
struct ResolvedLayer {
    const ToneLayer*    layer    = nullptr;  // ToneLayer の参照
    const HwPatch*      hwPatch  = nullptr;  // 解決済み HwPatch (通常のFMオペレータ系チップ)
    // サンプルベース音源系 (VOICE_PATCH_AWM等) の場合のみ設定される。
    // hwPatchとは排他 (どちらか一方のみ非nullptr)。既存チップドライバは
    // hwPatchのみを参照するため、このフィールド追加による影響はない。
    const SampleZonePatch* samplePatch = nullptr;
    int              deviceIndex = -1;    // devices[] インデックス
    // 解決済み SwPatch (パフォーマンスパッチ)。以前はResolvedPatch側に
    // Patch単位で1つだけ保持していたが、HwPatch自身がswBank/swProgを
    // 持つ設計に変更したため、レイヤー単位(HwPatch単位)の粒度に変更した
    // (2026年7月)。解決に失敗した場合もnullptrのままになるだけで、
    // HwPatch自体の発音は妨げられない(ソフトな失敗)。
    const SwPatch*   swPatch = nullptr;
    // ハードウェア制約による強制チャンネル(-1=制約なし)。
    // PatchManager::ResolvedTriple::forcedChからそのまま引き継がれる。
    // 詳細はPatchManager.hのResolvedTriple::forcedChコメント参照。
    int8_t           forcedCh = -1;
};

struct ResolvedPatch {
    const Patch*    patch   = nullptr;  // 元の Patch
    std::array<ResolvedLayer, MAX_TONE_LAYERS> layers;
    int layerCount = 0;

    bool isValid() const noexcept { return patch != nullptr; }
};

} // namespace fitom
