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
//  HwBankRegistry: チップ族 × バンク番号 → HwBank のマッピング
//
//  旧 CFITOMConfig の vOpmBank[] / vOpnBank[] 等を統合する。
//  チップ族 (VoiceGroup) をキーにしてバンクを引く。
// ================================================================
class HwBankRegistry {
public:
    // VoiceGroup ビットマスク (FITOMdefine.h の VOICE_GROUP_* に対応)
    using VoiceGroup = uint32_t;

    // バンクを登録・取得
    HwBank& getOrCreate(VoiceGroup group, int bankNo) {
        return banks_[group][bankNo];
    }
    const HwBank* find(VoiceGroup group, int bankNo) const {
        auto it = banks_.find(group);
        if (it == banks_.end()) return nullptr;
        auto it2 = it->second.find(bankNo);
        if (it2 == it->second.end()) return nullptr;
        return &it2->second;
    }

    // デバイス ID から VoiceGroup を解決 (旧 GetDeviceVoiceGroupMask 相当)
    static VoiceGroup groupFromDeviceId(uint32_t deviceId);

    // HwPatch の解決:
    //   指定の group/bank/prog から HwPatch を返す。
    //   見つからない場合は nullptr。
    const HwPatch* resolve(VoiceGroup group, int bankNo, int prog) const {
        const HwBank* b = find(group, bankNo);
        if (!b) return nullptr;
        const auto& p = b->get(prog);
        return p.isValid() ? &p : nullptr;
    }

private:
    std::unordered_map<VoiceGroup,
        std::unordered_map<int, HwBank>> banks_;
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

    // ─── SW パッチ参照 ────────────────────────────────────────────
    // SW パラメータはすべての ToneLayer で共有する（1パッチ1セット）。
    // 注意: hwBank/hwProg と同様、Patch プリセット (JSON) に静的に
    // 埋め込まれた値であり、ライブの MIDI CC からは供給されない。
    uint8_t swBank;   // SW バンク番号
    uint8_t swProg;   // SW プログラム番号

    // ─── パッチ共通パラメータ ─────────────────────────────────────
    uint8_t poly;     // ポリフォニー数 (0=チャンネルマップのデフォルト)

    Patch() noexcept
        : id(0xFFFFFFFFu), swBank(0), swProg(0), poly(0)
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
    const ToneLayer* layer    = nullptr;  // ToneLayer の参照
    const HwPatch*   hwPatch  = nullptr;  // 解決済み HwPatch
    int              deviceIndex = -1;    // devices[] インデックス
};

struct ResolvedPatch {
    const Patch*    patch   = nullptr;  // 元の Patch
    const SwPatch*  swPatch = nullptr;  // 解決済み SwPatch
    std::array<ResolvedLayer, MAX_TONE_LAYERS> layers;
    int layerCount = 0;

    bool isValid() const noexcept { return patch != nullptr; }
};

} // namespace fitom
