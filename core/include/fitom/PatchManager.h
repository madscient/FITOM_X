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
#include "fitom/DrumData.h"
#include "fitom/SccWaveData.h"
#include "fitom/PcmBankData.h"
#include "fitom/Log.h"
#include <vector>
#include <array>
#include <memory>
#include <filesystem>
#include <functional>
#include <string>

namespace fitom {

class FITOMConfig;  // デバイスリストへのアクセスに使用

// ================================================================
//  PatchManager
// ================================================================
class PatchManager {
public:
    explicit PatchManager();

    // ─── バンク管理 ───────────────────────────────────────────────

    HwBankRegistry&  hwRegistry() { return hwReg_; }
    // サンプルベース音源系 (VOICE_PATCH_AWM等) 専用。hwRegistry()とは
    // 完全に独立したレジストリ (HwPatch/HwBankには一切影響しない)。
    SampleZoneBankRegistry& sampleRegistry() { return sampleReg_; }
    SwBankRegistry&  swRegistry() { return swReg_; }

    // swBank/swProg(-1=参照なし)からSwPatchを解決する。-1の場合、
    // または指定先が存在しない場合はnullptrを返す(ソフトな失敗。
    // 呼び出し元はこれをエラー扱いせず、単にパフォーマンスパッチ
    // 無しとして扱う)。resolveTriple()がHwPatch自身のswBank/swProgを
    // 解決するために内部的に使うほか、CRhythmChがDrumNote側の上書き
    // (dn.swBank/swProg)を解決する際にも直接呼ばれる。
    const SwPatch* resolveSwPatch(int8_t swBank, int8_t swProg) const {
        if (swBank < 0 || swProg < 0) return nullptr;
        return swReg_.resolve(swBank, swProg);
    }

    // パッチバンクの取得・登録
    PatchBank& getPatchBank(int bankNo);
    const PatchBank* findPatchBank(int bankNo) const;
    // 登録済みのPatchBank番号一覧を昇順で返す(GUIのパッチピッカー
    // ダイアログ向け、通常モードのCC#32階層列挙用、2026年7月新設)。
    std::vector<int> listPatchBankNumbers() const;

    // ─── SysExによるHwPatchパラメータオーバーライド ────────────────
    // JSON文字列をパースし、targetへ差分マージする(存在するキーのみ
    // 上書き。id/name/builtinはこの経路では対象外、意図的に無視する)。
    // "ops"は0-4要素の可変長配列で、null または {} の要素は該当
    // オペレータをスキップする(現在値を変更しない)。
    // 呼び出し元は、対象がMIDIチャンネル単位のオーバーライドか
    // (CInstCh::mergeHwPatchOverride参照)、プリセットバンク直接編集か
    // (hwRegistry().findMutable()で得たHwPatchへ直接この関数を呼ぶ)を
    // 問わず、同じマージ処理を共有する。
    // JSON構文エラー時はfalseを返しtargetは変更しない(全体を無視、
    // 部分適用はしない)。errorOutが非nullなら詳細を格納する。
    bool mergeHwPatchFromJsonText(const std::string& jsonText, HwPatch& target,
                                   std::string* errorOut = nullptr) const;

    // SwPatch版。フィールドは"sw"(LWF/LFS/LFM/LFD/LFR/LFI/depth_cents)・
    // "ops"(0-4要素、null/{}スキップ)・"fine_transpose"。id/nameは対象外。
    // 詳細な規約はmergeHwPatchFromJsonText()と同じ。
    bool mergeSwPatchFromJsonText(const std::string& jsonText, SwPatch& target,
                                   std::string* errorOut = nullptr) const;

    // ─── プログラムチェンジ解決 ──────────────────────────────────

    // Patch + デバイス情報 → ResolvedPatch を構築する。
    // config はデバイス device_type 解決のために使う。
    ResolvedPatch resolve(const Patch& patch,
                          const FITOMConfig& config) const;

    // 簡易版: bank/prog → Patch → ResolvedPatch
    ResolvedPatch resolve(int patchBank, int prog,
                          const FITOMConfig& config) const;

    // バンクセレクトLSB直接指定モード: PatchBank/ToneLayerを経由せず、
    // voicePatchType + hwBank + hwProg から単層 ResolvedPatch を直接構築する。
    // storage は呼び出し側 (CInstCh) が寿命を保持する実体
    // (ResolvedPatch::patch はここを指す)。
    // SwPatch は常に nullptr (直接モードはベロシティ感度等を持たない)。
    ResolvedPatch resolveDirect(uint8_t voicePatchType, uint8_t hwBank, uint8_t hwProg,
                                const FITOMConfig& config, Patch& storage) const;

    // ─── バンクファイル I/O ───────────────────────────────────────

    // JSON 形式でバンクを読み込む
    // voicePatchType: バンク全体で共通の VoicePatchType (VOICE_PATCH_*)。
    // 省略時は 0 (未設定、後方互換用)。
    bool loadHwBankJson(const std::filesystem::path& path,
                        HwBankRegistry::VoiceGroup group, int bankNo,
                        uint8_t voicePatchType = 0);
    // OPLL ROM音色用のswPatchメタデータ専用バンクを読み込む
    // (profile.jsonのhw_banks[].role=="builtin_swpatch_meta"用、
    // 2026年7月新設)。通常のHwBankRegistryには登録せず、
    // opllBuiltinMetaBank_に直接保持する。
    bool loadOpllBuiltinMetaBankJson(const std::filesystem::path& path);
    bool loadSwBankJson(const std::filesystem::path& path, int bankNo);
    bool loadPatchBankJson(const std::filesystem::path& path, int bankNo);

    // サンプルベース音源系 (VOICE_PATCH_AWM等) 専用。HwBankRegistryとは
    // 完全に独立したロード経路 (hwRegistry()には一切影響しない)。
    bool loadSampleZoneBankJson(const std::filesystem::path& path,
                                 int bankNo, uint8_t voicePatchType = 0);

    // 旧 FITOM INI 形式 (レガシー互換)
    bool loadHwBankLegacy(const std::filesystem::path& path,
                          HwBankRegistry::VoiceGroup group, int bankNo);

    // JSON 形式でバンクを書き出す
    bool saveHwBankJson(const std::filesystem::path& path,
                        HwBankRegistry::VoiceGroup group, int bankNo) const;
    bool saveSampleZoneBankJson(const std::filesystem::path& path, int bankNo) const;
    bool saveSwBankJson(const std::filesystem::path& path, int bankNo) const;
    bool savePatchBankJson(const std::filesystem::path& path, int bankNo) const;

    // ─── ドラムバンク ────────────────────────────────────────────
    DrumBankRegistry& drumRegistry() { return drumReg_; }

    const DrumPatch* resolveDrum(int bankNo, int prog) const {
        return drumReg_.resolve(bankNo, prog);
    }

    bool loadDrumBankJson(const std::filesystem::path& path, int bankNo);
    bool saveDrumBankJson(const std::filesystem::path& path, int bankNo) const;

    // 新方式: プログラムチェンジ1つぶんのドラムキットを、独立した小さな
    // ファイル (*.drumkit.json) から直接ロードする。旧方式(1ファイルに
    // 全prog分のpatches[]を詰め込む loadDrumBankJson)は、ファイルが
    // 肥大化する問題があったため、ファイル分割を前提とした設計に変更。
    // ドラムバンクは常に固定バンク番号(0)を使う (CRhythmChはMIDI経由での
    // バンク切替をサポートしないため)。
    bool loadDrumKitJson(const std::filesystem::path& path, int prog);
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
    // voicePatchType + hwBank + hwProg の3つ組から、実際に発音可能な
    // (device, HwPatch または SamplePatch) を解決する共通ロジック。
    // resolve()の各ToneLayer処理、resolveDirect()の両方から呼ばれる
    // (旧実装ではほぼ同一のロジックが2箇所に重複していたため統一した)。
    //
    // voicePatchType == VOICE_PATCH_NONE(0) は常に失敗として扱う
    // (CC#0の実時間セマンティクスにおける「通常モード」=PatchBank参照に
    //  相当する値。もしこの関数がPatchBank参照も扱えるよう将来拡張
    //  された場合、ToneLayer同士が循環参照する経路を開いてしまう
    //  ため、この関数の入口で構造的に禁止しておく)。
    struct ResolvedTriple {
        int deviceIndex = -1;
        const HwPatch* hwPatch = nullptr;
        const SampleZonePatch* samplePatch = nullptr;
        const SwPatch* swPatch = nullptr;
        // ハードウェア制約により、この解決結果を発音する際に必ず特定の
        // デバイスチャンネルへ強制割り当て(assignCh)しなければならない
        // 場合に、そのチャンネル番号を保持する(-1=制約なし、通常通り
        // allocCh/DVAによる動的割り当てが可能)。ビルトインリズム音源
        // (resolveBuiltinRhythm参照)が現状唯一の設定元。
        int8_t forcedCh = -1;
        bool isValid() const { return deviceIndex >= 0; }
    };
    // logContext: ログメッセージの主語("layer=N" 等)。空文字なら
    // resolveDirect相当の文言にする。
    ResolvedTriple resolveTriple(uint8_t voicePatchType, uint8_t hwBank, uint8_t hwProg,
                                  const FITOMConfig& config,
                                  const std::string& logContext = "") const;

    // ─── OPLL系ROM音色専用の解決ロジック ──────────────────────────
    // voicePatchTypeがOPLLファミリー(OPLL/OPLLP/OPLLX/VRC7、OPLL2は
    // VOICE_PATCH_OPLLを共有するため区別不要)で、hwBank==0の場合、
    // 通常のHwBankRegistry検索を経由せず、hwProgの上位3bit/下位4bitから
    // 直接HwPatchを合成する。バンク0はROM音色専用の予約領域であり、
    // JSON(プリセット)による定義は不可 (resolveTripleが常にこちらを
    // 優先するため、たとえJSONでロードされていても参照されない)。
    //   上位3bit(hwProg>>4): 0=OPLL, 1=OPLLX, 2=OPLLP, 3=VRC7 (4-7は無効)
    //   下位4bit(hwProg&0xF): 0=無音(ユーザー音色との衝突回避のため
    //                         意図的に予約)、1-15=ROM音色インデックス
    ResolvedTriple resolveOpllRomVoice(uint8_t hwProg, const FITOMConfig& config,
                                        const std::string& logContext) const;

    // ─── 内蔵リズム音源専用の解決ロジック ──────────────────────────
    // voicePatchType == VOICE_PATCH_BUILTIN_RHYTHM(0x70)の場合に
    // resolveTriple()から呼ばれる。
    //   chipSel(hwBank相当): 対象チップを選ぶ。既存のVOICE_PATCH_*定数
    //     を再利用する(VOICE_PATCH_OPN2=OPNA、VOICE_PATCH_OPLL=OPLL)。
    // 通常のVoicePatchTypeベースルーティングでは、COPNARhythm/
    // COPLLRhythmはdeviceTypeToVoicePatchType()がVOICE_PATCH_NONEを
    // 返すため到達不能 (findDeviceIndexByDeviceType()で直接検索する)。
    //
    // 「楽器番号=チャンネル番号」というハードウェア上の制約があるため、
    // hwProg(patch_prog相当、CInstCh::progChangeのProgram Changeと同じ
    // 入力経路)をそのままデバイスチャンネル番号として解釈し、
    // ResolvedTriple::forcedChに設定する。範囲外のhwProgは無効な
    // チャンネルとして扱い、警告を出してforcedCh=-1のまま返す
    // (呼び出し元は通常のDVAにフォールバックせず、発音自体を
    // スキップする — 誤った楽器が鳴るより無音の方が安全)。
    // これにより、DrumNote(CRhythmCh)経由だけでなく、CInstChの通常の
    // Program Changeからもビルトインリズムの個別楽器を選択できる
    // (VoiceData選択→デバイス確定、の原則をここでも一貫させる)。
    ResolvedTriple resolveBuiltinRhythm(uint8_t chipSel, uint8_t hwProg,
                                         const FITOMConfig& config,
                                         const std::string& logContext) const;

    // resolveOpllRomVoice()が返すHwPatchの実体。ResolvedTriple::hwPatchは
    // ポインタのため、呼び出し元が使い続けられるよう安定した記憶域に
    // 保持する。[variantSel(0-3)][instIndex(0-15)]。プリセットフラグ
    // (ext.ALG_EXT=1)とINSTナンバー(hw.ALG)以外のフィールドはOPLLドライバ
    // 側で無視されるため未設定のままでよい。PatchManagerコンストラクタで
    // 一括初期化する。
    mutable std::array<std::array<HwPatch, 16>, 4> opllRomPatches_{};
    void initOpllRomPatches();

    // OPLL ROM音色用のswPatchメタデータ専用バンク(2026年7月新設)。
    // profile.jsonのhw_banks[].role=="builtin_swpatch_meta"で指定
    // されたバンクが、通常のHwBankRegistry検索を経由せず、ここに
    // 直接保持される。resolveOpllRomVoice()が
    // HwBank::findByBuiltinRef()で参照する。未設定ならnullptrのまま
    // (通常通りswPatch適用なしで動作する、ソフトな失敗)。
    HwBank opllBuiltinMetaBank_;
    bool   hasOpllBuiltinMetaBank_ = false;

    HwBankRegistry hwReg_;
    SampleZoneBankRegistry sampleReg_;
    SwBankRegistry swReg_;
    DrumBankRegistry drumReg_;
    SccWaveRegistry  sccWaveReg_;
    PcmBankRegistry  pcmReg_;
    std::unordered_map<int, PatchBank> patchBanks_;
    ProgressCallback progressCb_;

    void reportProgress(const std::string& msg) const {
        if (progressCb_) progressCb_(msg);
    }
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
