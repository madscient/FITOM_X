// fitom/PatchManager.cpp
// パッチ解決・バンク管理の実装

#include "fitom/PatchManager.h"
#include "fitom/PcmBankData.h"
#include "fitom/Config.h"
#include "fitom/DeviceFactory.h"
#include "fitom/FITOMdefine.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <unordered_map>

namespace fitom {

using json = nlohmann::json;

// ================================================================
//  HwBankRegistry: デバイス ID → VoiceGroup 解決
// ================================================================

HwBankRegistry::VoiceGroup HwBankRegistry::groupFromDeviceId(uint32_t deviceId)
{
    // FITOMdefine.h の DEVICE_* → VOICE_GROUP_* への対応
    // 旧 CFITOM::GetDeviceVoiceGroupMask 相当
    switch (deviceId & 0xFF) {
    case DEVICE_OPM:  case DEVICE_OPP:  case DEVICE_OPZ:  case DEVICE_OPZ2:
        return VOICE_GROUP_OPM;
    case DEVICE_OPN:  case DEVICE_OPN2: case DEVICE_OPNA: case DEVICE_OPN3L:
    case DEVICE_OPNB: case DEVICE_OPNC: case DEVICE_OPN2C: case DEVICE_OPN2L:
    case DEVICE_2610B: case DEVICE_F286: case DEVICE_OPN3:
        return VOICE_GROUP_OPNA;
    case DEVICE_OPL:  case DEVICE_OPL2: case DEVICE_Y8950:
        return VOICE_GROUP_OPL2;
    case DEVICE_OPL3: case DEVICE_OPN3_L3:
        return VOICE_GROUP_OPL3;
    case DEVICE_OPLL: case DEVICE_OPLL2: case DEVICE_OPLLP: case DEVICE_OPLLX:
        return VOICE_GROUP_OPLL;
    case DEVICE_SSG:  case DEVICE_PSG:  case DEVICE_SSGL: case DEVICE_SSGLP:
    case DEVICE_SSGS: case DEVICE_EPSG: case DEVICE_DCSG: case DEVICE_SCC:
    case DEVICE_SCCP: case DEVICE_SAA:  case DEVICE_DSG:
        return VOICE_GROUP_PSG;
    case DEVICE_OPL4: case DEVICE_OPL4ML: case DEVICE_OPL4ML2:
        return VOICE_GROUP_OPL4;
    case DEVICE_ADPCM: case DEVICE_ADPCMA: case DEVICE_ADPCMB:
    case DEVICE_PCMD8: case DEVICE_MA1: case DEVICE_MA2:
    case DEVICE_MA3:   case DEVICE_MA5:  case DEVICE_MA7:
        return VOICE_GROUP_PCM;
    case DEVICE_RHYTHM:
        return VOICE_GROUP_RHYTHM;
    default:
        return VOICE_GROUP_NONE;
    }
}

// ================================================================
//  PatchManager
// ================================================================

PatchBank& PatchManager::getPatchBank(int bankNo)
{
    return patchBanks_[bankNo];
}

const PatchBank* PatchManager::findPatchBank(int bankNo) const
{
    auto it = patchBanks_.find(bankNo);
    return (it != patchBanks_.end()) ? &it->second : nullptr;
}

namespace {

// ================================================================
//  VoicePatchType フォールバック
//
//  voicePatchTypeToVoiceGroup() (Config.cpp) はバンクファイルの名前空間
//  分類が目的であり、「音色データがレジスタレベルで互換性を持つか」を
//  意味しない (例: VOICE_GROUP_PSGはSSG/AY8930/DCSG/SAA1099/SCCを一括り
//  にしているが、ALG/WSフィールドの意味がチップごとに全く異なり
//  互換性が無い)。
//
//  フォールバック可否の判定は、各チップドライバファイルに実装された
//  静的関数 (copnAcceptsFallback等、DeviceFactory::acceptsFallback経由で
//  呼ばれる) に委ねる。判定にはHwPatchの内容 (拡張パラメータの使用有無、
//  プリセット/ユーザー区別等) が必要になる場合があるため、チップ
//  ドライバ自身の知識として実装するのが最も正確 (静的テーブルでは
//  「OPZ→OPMは拡張パラメータ未使用時のみ」のような条件を表現できない)。
//
//  フォールバックは「同じHwPatchデータを、要求されたVoicePatchTypeとは
//  別の接続デバイスで鳴らす」という意味であり、HwBank/HwPatchの検索
//  そのものは常にlayer本来のvoicePatchType基準で行う (データの検索先は
//  変えず、実際に発音を担当するデバイスの選択だけが変わる)。
// ================================================================

// 接続されている全デバイスに対して DeviceFactory::acceptsFallback を
// 順に問い合わせ、受け入れ可能な最初の1つのインデックスを返す
// (-1 = 見つからない)。usedVptにそのデバイスの実際のVoicePatchTypeを返す。
int findFallbackDeviceIndex(const FITOMConfig& config, uint8_t sourceVoicePatchType,
                             const HwPatch& patch, uint8_t& usedVpt) {
    int count = config.getDeviceCount();
    for (int i = 0; i < count; ++i) {
        uint32_t deviceType = config.getDeviceType(i);
        if (DeviceFactory::acceptsFallback(deviceType, sourceVoicePatchType, patch)) {
            usedVpt = FITOMConfig::deviceTypeToVoicePatchType(deviceType);
            return i;
        }
    }
    return -1;
}

} // namespace

ResolvedPatch PatchManager::resolve(const Patch& patch,
                                     const FITOMConfig& config) const
{
    ResolvedPatch result;
    result.patch = &patch;

    // 各 ToneLayer の HwPatch を解決 (共通ロジックはresolveTriple()に集約、
    // resolveDirect()と共有する)。voicePatchTypeに一致するデバイスが
    // 見つからない、またはHwBankのタグと不一致の場合、そのレイヤーだけを
    // スキップする (Patch全体は無効にしない。確保できたレイヤーのみ発音する)。
    // SwPatch(パフォーマンスパッチ)は、以前はPatch単位で1つだけ
    // 解決していたが、HwPatch自身がswBank/swProgを持つ設計に変更した
    // ため、resolveTriple()内でHwPatchごとに自動解決される
    // (レイヤーごとに異なるパフォーマンスパッチを持ちうる、2026年7月)。
    for (int i = 0; i < MAX_TONE_LAYERS; ++i) {
        const auto& layer = patch.layers[i];
        if (!layer.isActive()) continue;

        auto rt = resolveTriple(layer.voicePatchType, layer.hwBank, layer.hwProg,
                                 config, "layer=" + std::to_string(i));
        if (!rt.isValid()) continue;

        ResolvedLayer rl;
        rl.layer       = &layer;
        rl.deviceIndex = rt.deviceIndex;
        rl.hwPatch     = rt.hwPatch;
        rl.samplePatch = rt.samplePatch;
        rl.swPatch     = rt.swPatch;
        rl.forcedCh    = rt.forcedCh;
        result.layers[result.layerCount++] = rl;
    }

    return result;
}

ResolvedPatch PatchManager::resolveDirect(uint8_t voicePatchType, uint8_t hwBank, uint8_t hwProg,
                                          const FITOMConfig& config, Patch& storage) const
{
    auto rt = resolveTriple(voicePatchType, hwBank, hwProg, config);
    if (!rt.isValid()) return {};

    // storage (呼び出し元が寿命を保持) に単層Patchを構築する。
    // (Patch::fromSingleLayerはvoicePatchType/hwBank/hwProgの記録用の
    //  枠組みとしてのみ使われる。内蔵リズム音源はresolveBuiltinRhythm()
    //  が常に空のダミーHwPatchを返すため、この分岐には到達しない)。
    if (rt.samplePatch) {
        storage = Patch::fromSingleLayer(*rt.samplePatch, voicePatchType, hwBank, hwProg);
    } else if (rt.hwPatch) {
        storage = Patch::fromSingleLayer(*rt.hwPatch, voicePatchType, hwBank, hwProg);
    } else {
        static const HwPatch kDummyPatch{};
        storage = Patch::fromSingleLayer(kDummyPatch, voicePatchType, hwBank, hwProg);
    }

    ResolvedPatch result;
    result.patch      = &storage;
    ResolvedLayer rl;
    rl.layer       = &storage.layers[0];
    rl.hwPatch     = rt.hwPatch;
    rl.samplePatch = rt.samplePatch;
    rl.deviceIndex = rt.deviceIndex;
    rl.swPatch     = rt.swPatch;   // 直接モードでも、HwPatch自身が
                                    // swBank/swProgを持てば適用される
                                    // (2026年7月の設計変更、旧仕様は
                                    //  「直接モードはSwPatchなし」だった)
    rl.forcedCh    = rt.forcedCh;
    result.layers[0]  = rl;
    result.layerCount = 1;
    return result;
}

// voicePatchType + hwBank + hwProg の3つ組から、実際に発音可能な
// (device, HwPatch または SamplePatch) を解決する共通ロジック。
// resolve()の各ToneLayer処理、resolveDirect()の両方から呼ばれる。
PatchManager::PatchManager()
{
    initOpllRomPatches();
}

// [variantSel][instIndex] の全64エントリを事前に構築する。
// OPLLドライバ(updateVoice)はプリセット音色の場合、ext.ALG_EXT(bit0)と
// hw.ALG(下位4bit=INSTナンバー)以外のフィールドを一切参照しないため、
// この2フィールドだけを設定すればよい(他はデフォルト値のまま)。
void PatchManager::initOpllRomPatches()
{
    // ROM音色名。出典: https://github.com/plgDavid/misc/wiki/Copyright-free-OPLL(x)-ROM-patches
    // (耳コピによる非公式な近似データ。著作権フリーを謳っているが、
    //  正確な公式名称ではない可能性がある点に留意)。
    // index[0]はINSTナンバー0(ユーザー音色、このバンクでは常に無音)用の
    // ダミーで未使用。1-15が実際のROM音色名に対応する。
    static const char* const kNames[4][16] = {
        // 0: OPLL (YM2413) / OPLL2
        { "", "Violin", "Guitar", "Piano", "Flute", "Clarinet", "Oboe",
          "Trumpet", "Organ", "Horn", "Synthesizer", "Harpsichord",
          "Vibraphone", "Synthesizer Bass", "Acoustic Bass", "Electric Guitar" },
        // 1: OPLLX (YM2423)
        { "", "Strings", "Guitar", "Electric Guitar", "Electric Piano 2",
          "Flute", "Marimba", "Trumpet", "Harmonica", "Tuba",
          "Synth Brass 2", "Short Saw", "Vibraphone", "Electric Guitar 2",
          "Synth Bass 2", "Sitar" },
        // 2: OPLLP (YMF281)
        { "", "Clarinet", "Synth Bass", "Piano", "Flute", "Square Wave",
          "Space Oboe", "Trumpet", "Wow Bell", "Electric Guitar", "Vibes",
          "Bass", "Vibraphone", "Vibrato Bell", "Click Sine", "Noise and Tone" },
        // 3: VRC7
        { "", "Buzzy Bell", "Guitar", "Wurly", "Flute", "Clarinet", "Synth",
          "Trumpet", "Organ", "Bells", "Vibes", "Vibraphone", "Tutti",
          "Fretless", "Synth Bass", "Sweep" },
    };

    for (int variantSel = 0; variantSel < 4; ++variantSel) {
        for (int instIndex = 0; instIndex < 16; ++instIndex) {
            HwPatch& p = opllRomPatches_[variantSel][instIndex];
            p = HwPatch{};
            // id: バンク番号は常に0(ROM専用予約バンク)、prog番号はhwProgの
            // 生値(variantSel<<4 | instIndex)をそのまま使う。isValid()が
            // 正しくtrueを返すよう、デフォルトの0xFFFFFFFFuから変更する。
            p.id = (static_cast<uint32_t>(variantSel) << 4) | static_cast<uint32_t>(instIndex);
            std::strncpy(p.name, kNames[variantSel][instIndex], sizeof(p.name) - 1);
            p.ext.ALG_EXT = 1;                          // プリセット選択フラグ
            p.hw.ALG      = static_cast<uint8_t>(instIndex & 0xF); // INSTナンバー
        }
    }
}

// OPLL系ROM音色専用の解決ロジック。voicePatchTypeがOPLLファミリーの
// いずれかで、hwBank==0の場合にresolveTriple()から呼ばれる。
PatchManager::ResolvedTriple PatchManager::resolveOpllRomVoice(
    uint8_t hwProg, const FITOMConfig& config, const std::string& logContext) const
{
    ResolvedTriple result;

    uint8_t variantSel = static_cast<uint8_t>((hwProg >> 4) & 0x7);
    uint8_t instIndex  = static_cast<uint8_t>(hwProg & 0xF);

    // 下位4bit=0は無音 (ユーザー音色(INST=0)との衝突を避けるため、
    // このROM音色専用バンクでは意図的に予約し、何も鳴らさない)。
    if (instIndex == 0) return result;

    static constexpr uint8_t kVariantMap[8] = {
        VOICE_PATCH_OPLL,   // 0 (OPLL2もVOICE_PATCH_OPLLを共有するため区別不要)
        VOICE_PATCH_OPLLX,  // 1
        VOICE_PATCH_OPLLP,  // 2
        VOICE_PATCH_VRC7,   // 3
        0, 0, 0, 0          // 4-7: 未定義
    };
    uint8_t actualVpt = kVariantMap[variantSel];
    if (actualVpt == 0) {
        FITOM_LOG_WARN((logContext.empty() ? "resolveDirect:" : ("resolve: " + logContext))
            << " OPLL ROM voice: undefined variant selector=" << (int)variantSel
            << " (prog=0x" << std::hex << (int)hwProg << ")");
        return result;
    }

    // ROM音色はチップごとに実データが全く異なるため、フォールバックは
    // 行わない (opllFamilyAcceptsFallbackがプリセット音色のフォールバック
    // を拒否するのと同じ方針)。要求された具体的なチップが接続されて
    // いなければ、そのまま失敗として扱う。
    int deviceIndex = config.findDeviceIndexByVoicePatchType(actualVpt);
    if (deviceIndex < 0) {
        FITOM_LOG_WARN((logContext.empty() ? "resolveDirect:" : ("resolve: " + logContext))
            << " OPLL ROM voice: voicePatchType=0x" << std::hex << (int)actualVpt
            << " — no matching device (ROM音色はフォールバック非対応)");
        return result;
    }

    result.deviceIndex = deviceIndex;
    result.hwPatch = &opllRomPatches_[variantSel][instIndex];
    // ROM音色自体のswBank/swProgは常に-1(未設定)のため(initOpllRomPatches
    // 参照)、代わりにopllBuiltinMetaBank_(profile.jsonのhw_banks[].role=
    // "builtin_swpatch_meta"で指定された専用バンク)を、(variantSel,
    // instIndex)で検索してswPatchを解決する(2026年7月新設)。
    // 未設定・未一致ならnullptrのまま(ソフトな失敗、ROM音色自体の発音は
    // 妨げられない)。
    if (hasOpllBuiltinMetaBank_) {
        const HwPatch* metaPatch = opllBuiltinMetaBank_.findByBuiltinRef(
            static_cast<int8_t>(variantSel), static_cast<int8_t>(instIndex));
        if (metaPatch) {
            result.swPatch = resolveSwPatch(metaPatch->swBank, metaPatch->swProg);
        }
    }
    return result;
}

// 内蔵リズム音源専用の解決ロジック。voicePatchType ==
// VOICE_PATCH_BUILTIN_RHYTHM(0x70)の場合にresolveTriple()から呼ばれる。
// 「対象チップ(デバイス)の解決」に加え、hwProgをそのままチャンネル
// (=楽器)番号として検証し、ResolvedTriple::forcedChに設定する。
PatchManager::ResolvedTriple PatchManager::resolveBuiltinRhythm(
    uint8_t chipSel, uint8_t hwProg, const FITOMConfig& config,
    const std::string& logContext) const
{
    ResolvedTriple result;
    std::string ctx = logContext.empty()
        ? std::string("resolveDirect:")
        : ("resolve: " + logContext);

    uint32_t targetDeviceType = DEVICE_NONE;
    if (chipSel == VOICE_PATCH_OPN2) {       // OPNA
        targetDeviceType = DEVICE_OPNA_RHY;
    } else if (chipSel == VOICE_PATCH_OPLL) { // OPLL
        targetDeviceType = DEVICE_OPLL_RHY;
    } else {
        // OPL(COPLRhythm)は現状未実装、その他の値も無効。
        FITOM_LOG_WARN(ctx << " builtin-rhythm: undefined chip selector=0x"
            << std::hex << (int)chipSel);
        return result;
    }

    int deviceIndex = config.findDeviceIndexByDeviceType(targetDeviceType);
    if (deviceIndex < 0) {
        FITOM_LOG_WARN(ctx << " builtin-rhythm: deviceType=0x" << std::hex << targetDeviceType
            << " — no matching device connected");
        return result;
    }

    ISoundDevice* dev = config.getDevice(deviceIndex);
    const uint8_t chCount = dev ? dev->getChCount() : 0;
    if (hwProg >= chCount) {
        FITOM_LOG_WARN(ctx << " builtin-rhythm: prog=" << (int)hwProg
            << " is out of range for deviceType=0x" << std::hex << targetDeviceType
            << std::dec << " (chCount=" << (int)chCount << ") — no instrument selected");
        return result; // forcedCh=-1のまま、deviceIndexも未設定=無効
    }

    result.deviceIndex = deviceIndex;
    result.forcedCh = static_cast<int8_t>(hwProg);
    // COPNARhythm/COPLLRhythmはHwPatchの中身を一切参照しないが、
    // assignCh()はpatch/samplePatchが両方nullptrだとupdateVoice()自体を
    // 呼ばない(音量/パン初期化が行われなくなる)ため、空のダミー
    // HwPatchへのポインタを設定しておく(中身は使われないので内容は
    // 空のままでよい)。
    static const HwPatch kEmptyRhythmPatch{}; // swBank/swProgは常に-1のまま
    result.hwPatch = &kEmptyRhythmPatch;
    // このHwPatchはチップ全体で共有されるダミーのため、楽器(prog)
    // ごとの区別ができず、HwPatch自身にswPatchを持たせる意味が無い
    // (常に-1でresult.swPatchはnullptrのまま)。楽器ごとに異なる
    // パフォーマンスパッチを与えたい場合は、DrumNote::swBank/swProg
    // による上書きを使う (CRhythmCh::applyNoteOn参照)。
    return result;
}

PatchManager::ResolvedTriple PatchManager::resolveTriple(
    uint8_t voicePatchType, uint8_t hwBank, uint8_t hwProg,
    const FITOMConfig& config, const std::string& logContext) const
{
    ResolvedTriple result;

    // MSB=0(VOICE_PATCH_NONE)は常に失敗として扱う。CC#0の実時間
    // セマンティクスにおける「通常モード」(PatchBank参照)に相当する
    // 値であり、この関数がPatchBank参照も扱えるよう将来拡張された
    // 場合、ToneLayer同士が循環参照する経路を開いてしまうため、
    // この入口で構造的に禁止しておく。
    if (voicePatchType == VOICE_PATCH_NONE) return result;

    // 内蔵リズム音源専用バンク: hwBank(CC#32相当)が対象チップを、
    // hwProg(ProgChg相当)がそのチップ内の楽器番号を選ぶ。通常の
    // HwBankRegistry検索を一切経由しない。
    if (voicePatchType == VOICE_PATCH_BUILTIN_RHYTHM) {
        return resolveBuiltinRhythm(hwBank, hwProg, config, logContext);
    }

    // OPLL系ROM音色専用バンク: バンク0はROM音色専用の予約領域であり、
    // 通常のHwBankRegistry検索(JSONプリセット)を経由しない。
    // (OPLL2はVOICE_PATCH_OPLLを共有するため、この判定に個別追加は不要)
    if (hwBank == 0 &&
        (voicePatchType == VOICE_PATCH_OPLL || voicePatchType == VOICE_PATCH_OPLLP ||
         voicePatchType == VOICE_PATCH_OPLLX || voicePatchType == VOICE_PATCH_VRC7)) {
        return resolveOpllRomVoice(hwProg, config, logContext);
    }

    std::string ctx = logContext.empty()
        ? std::string("resolveDirect:")
        : ("resolve: " + logContext);

    if (isSampleBasedVoicePatchType(voicePatchType)) {
        // サンプルベース音源系 (ADPCM-B/ADPCM-A/PCMD8/AWM):
        // HwBankRegistryではなくSampleZoneBankRegistryを検索する。
        // HwPatchが存在しないため、フォールバック機構
        // (findFallbackDeviceIndex、HwPatchの内容を要求する) は使わず、
        // 厳密一致のみを試す。サンプルベース音源系は現状フォールバック
        // 元/先になる想定がないため実用上の制約は小さい。
        const SampleZonePatch* samplePatch = sampleReg_.resolve(hwBank, hwProg);
        if (!samplePatch) {
            FITOM_LOG_WARN(ctx << " bank=" << (int)hwBank << " prog=" << (int)hwProg
                << " SampleZonePatch not found");
            return result;
        }
        int deviceIndex = config.findDeviceIndexByVoicePatchType(voicePatchType);
        if (deviceIndex < 0) {
            FITOM_LOG_WARN(ctx << " voicePatchType=0x" << std::hex << (int)voicePatchType
                << " — no matching device (サンプルベース音源系はフォールバック非対応)");
            return result;
        }
        result.deviceIndex = deviceIndex;
        result.samplePatch = samplePatch;
        return result;
    }

    // データ (HwBank/HwPatch) は常に要求されたvoicePatchType基準で検索する。
    // フォールバック可否の判定 (OPLLファミリーのプリセット/ユーザー判定等)
    // にHwPatchの内容が必要なため、デバイス検索より先にデータを解決する。
    auto group = FITOMConfig::voicePatchTypeToVoiceGroup(voicePatchType);
    const HwBank* bank = hwReg_.find(group, hwBank);
    if (!bank || bank->voicePatchType != voicePatchType) {
        FITOM_LOG_WARN(ctx << " bank=" << (int)hwBank
            << " voicePatchType mismatch (bank=" << (bank ? bank->voicePatchType : 0)
            << ", requested=" << (int)voicePatchType << ")");
        return result;
    }

    const HwPatch* hwPatch = &bank->get(hwProg);
    if (!hwPatch->isValid()) {
        FITOM_LOG_WARN(ctx << " HwPatch not found: bank=" << (int)hwBank
            << " prog=" << (int)hwProg);
        return result;
    }

    // デバイス検索: まず厳密一致を試し、無ければ接続済み全デバイスに
    // フォールバック受け入れ可否を問い合わせる。見つかった最初の
    // 1つだけを採用する。
    int deviceIndex;
    uint8_t usedVpt;
    if (isPsgFamilyVoicePatchType(voicePatchType)) {
        // PSG系共有バンク(2026年7月新設): バンク検索は常に共有の
        // 入口(VOICE_PATCH_SSG=0x40)で行うが、実際のデバイス解決は
        // パッチごとのext.targetVoicePatchType(未設定なら
        // config.getPsgFallbackChip())で行う。「音色がデバイスを
        // 選択する」原則を、バンク検索(常に0x40)とデバイス解決
        // (パッチごとに異なりうる)の2段階に分離することで、異なる
        // PSGチップ間で誤ってspanning(mergeSpannableDevices)されて
        // しまう事態を避けている。フォールバック機構
        // (findFallbackDeviceIndex)はチップ間のケーパビリティ差が
        // 大きく単純な互換性判定が難しいため使わず、明示的な
        // targetVoicePatchType指定とプロファイル設定のフォールバック
        // 先切り替えのみで対応する。
        uint8_t realVpt = hwPatch->ext.targetVoicePatchType;
        if (realVpt == 0) realVpt = config.getPsgFallbackChip();
        if (!isPsgFamilyVoicePatchType(realVpt)) {
            FITOM_LOG_WARN(ctx << " bank=" << (int)hwBank << " prog=" << (int)hwProg
                << " ext.targetVoicePatchType=0x" << std::hex << (int)realVpt
                << " is not a valid PSG family type");
            return result;
        }
        deviceIndex = config.findDeviceIndexByVoicePatchType(realVpt);
        usedVpt = realVpt;
    } else {
        deviceIndex = config.findDeviceIndexByVoicePatchType(voicePatchType);
        usedVpt = voicePatchType;
        if (deviceIndex < 0) {
            deviceIndex = findFallbackDeviceIndex(config, voicePatchType, *hwPatch, usedVpt);
        }
    }
    if (deviceIndex < 0) {
        FITOM_LOG_WARN(ctx << " voicePatchType=0x" << std::hex << (int)voicePatchType
            << " — no matching device (fallback exhausted)");
        return result;
    }
    if (usedVpt != voicePatchType) {
        if (isPsgFamilyVoicePatchType(voicePatchType)) {
            // PSG系共有バンク: これは切断時のフォールバックではなく、
            // パッチのext.targetVoicePatchType(またはpsg_fallback_chip)
            // による、意図的なデバイス選択の結果。
            FITOM_LOG_INFO(ctx << " PSG共有バンク(0x" << std::hex << (int)voicePatchType
                << ")のパッチ指定により、対象チップ0x" << (int)usedVpt << "を選択");
        } else {
            FITOM_LOG_INFO(ctx << " voicePatchType=0x" << std::hex << (int)voicePatchType
                << " not connected, falling back to 0x" << (int)usedVpt);
        }
    }

    result.deviceIndex = deviceIndex;
    result.hwPatch = hwPatch;
    // HwPatch自身が指定するパフォーマンスパッチ(SwPatch)を解決する。
    // -1(参照なし)、または指定先が見つからない場合はnullptrのまま
    // (ソフトな失敗。hwPatch自体の発音は妨げられない)。
    result.swPatch = resolveSwPatch(hwPatch->swBank, hwPatch->swProg);

    // COPLRhythm(OPL系内蔵リズム)は、COPNARhythm/COPLLRhythmと同じ
    // 「チャンネル番号=楽器」というハードウェア制約を持つ(実機レジスタ
    // 0xBDのビット位置に楽器番号がそのまま対応する、OPL_new.cppの
    // COPLRhythmクラス冒頭コメント参照)。ただしOPL_RHYはHwBankから
    // 「任意のHwPatch(音色)を指定可能」という設計のため、hwProg(格納
    // スロット番号)をそのままチャンネル番号に使うことはできない
    // (1チャンネルにつきHwBankスロットを1つしか使えなくなってしまう)。
    // 代わりに、HwPatch自身が持つext.rhythmCh(対象チャンネル番号、
    // HwPatch作成時に楽器ごとに指定)を参照する。未設定(0xFF)または
    // 範囲外は解決失敗として扱う(誤った楽器が鳴るより無音の方が安全、
    // という判断は0x70と同じ)。
    if (voicePatchType == VOICE_PATCH_OPL_RHY) {
        ISoundDevice* dev = config.getDevice(deviceIndex);
        const uint8_t chCount = dev ? dev->getChCount() : 0;
        const uint8_t rhythmCh = hwPatch->ext.rhythmCh;
        if (rhythmCh >= chCount) {
            FITOM_LOG_WARN(ctx << " OPL_RHY: bank=" << (int)hwBank
                << " prog=" << (int)hwProg << " ext.rhythmCh=" << (int)rhythmCh
                << " is unset or out of range (chCount=" << (int)chCount << ")");
            return ResolvedTriple{}; // 無効化して返す(無音)
        }
        result.forcedCh = static_cast<int8_t>(rhythmCh);
    }

    return result;
}


ResolvedPatch PatchManager::resolve(int patchBankNo, int prog,
                                     const FITOMConfig& config) const
{
    const PatchBank* pb = findPatchBank(patchBankNo);
    if (!pb) {
        FITOM_LOG_WARN("PatchBank " << patchBankNo << " not found");
        return {};
    }
    const Patch& p = pb->get(prog);
    if (!p.isValid()) {
        FITOM_LOG_WARN("Patch bank=" << patchBankNo << " prog=" << prog << " is empty");
        return {};
    }
    return resolve(p, config);
}

// ================================================================
//  JSON 変換ユーティリティ
// ================================================================

namespace {

// FmHwOp ↔ JSON
json hwOpToJson(const FmHwOp& op) {
    return json{
        {"AR",op.AR},{"DR",op.DR},{"SL",op.SL},{"SR",op.SR},{"RR",op.RR},
        {"TL",op.TL},{"KSR",op.KSR},{"KSL",op.KSL},
        {"MUL",op.MUL},{"DT1",op.DT1},{"DT2",op.DT2},{"FXV",op.FXV},
        {"AM",op.AM},{"VIB",op.VIB},{"EGT",op.EGT},{"WS",op.WS},
        {"REV",op.REV},{"EGS",op.EGS},{"DT3",op.DT3}
    };
}
void jsonToHwOp(const json& j, FmHwOp& op) {
    auto g = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g("AR",op.AR); g("DR",op.DR); g("SL",op.SL); g("SR",op.SR); g("RR",op.RR);
    g("TL",op.TL); g("KSR",op.KSR); g("KSL",op.KSL);
    g("MUL",op.MUL); g("DT1",op.DT1); g("DT2",op.DT2);
    if (j.contains("FXV")) op.FXV = j["FXV"].get<int16_t>();
    g("AM",op.AM); g("VIB",op.VIB); g("EGT",op.EGT); g("WS",op.WS);
    g("REV",op.REV); g("EGS",op.EGS); g("DT3",op.DT3);
}

// voicePatchTypeから、そのチップが使うオペレータ数(1/2/4)を判定する。
// docs/chip-driver-architecture.mdの「5. VoicePatchType 対応表」参照。
// HwBank保存時(hwPatchToJson)に、実際に意味のあるオペレータ数分だけ
// JSON出力するために使う(2026年7月〜。以前は常に4要素固定で出力して
// おり、2op/1opチップでも無意味なダミーデータが書き出されていた)。
// ADPCM/AWM系はHwPatchを使わない(SampleZonePatchを使う別スキーマ)ため
// この関数の対象外。
static int operatorCountForVoicePatchType(uint8_t vpt) {
    switch (vpt) {
    case VOICE_PATCH_SSG: case VOICE_PATCH_EPSG:
    case VOICE_PATCH_DCSG: case VOICE_PATCH_SAA:
    case VOICE_PATCH_SCC:
        return 1;
    case VOICE_PATCH_OPL: case VOICE_PATCH_OPL2: case VOICE_PATCH_OPL3_2:
    case VOICE_PATCH_OPLL: case VOICE_PATCH_OPLLP:
    case VOICE_PATCH_OPLLX: case VOICE_PATCH_VRC7:
        return 2;
    // OPL系内蔵リズムチャンネル: BD(バスドラム)は2オペレータの通常FM
    // ボイス、HH/SD/TOM/CYMは1オペレータのみ使う単発音のため、パッチ
    // ごとに実際のオペレータ数が異なる(混在する)。保存時はバンク単位で
    // 1つの代表値しか指定できないため、情報欠落を避けるためBDの
    // 要求(2)を代表値とする(HH等の保存時は未使用のhwOp[1]が0埋めで
    // 余分に出力されるだけで実害はない、2026年7月)。
    case VOICE_PATCH_OPL_RHY:
        return 2;
    case VOICE_PATCH_OPN: case VOICE_PATCH_OPN2:
    case VOICE_PATCH_OPM: case VOICE_PATCH_OPZ: case VOICE_PATCH_OPZ2:
    case VOICE_PATCH_OPL3:
        return 4;
    default:
        // SD1/MA3/MA5/MA7(未実装、将来のオペレータ数未確定)や、その他
        // 未知の値は、情報欠落を避けるため安全側のMAX_HW_OPSを返す。
        return MAX_HW_OPS;
    }
}

json hwPatchToJson(const HwPatch& p, uint8_t voicePatchType) {
    static constexpr const char* kBuiltinTypeNames[4] = {"OPLL", "OPLLX", "OPLLP", "VRC7"};
    json out;
    if (p.builtin.isValid()) {
        // builtin参照専用エントリ: ops[]は出力しない(排他)。
        out = json{
            {"id",p.id},{"name",p.name},
            {"builtin", json{
                {"patch_type", (p.builtin.patchType >= 0 && p.builtin.patchType < 4)
                    ? kBuiltinTypeNames[p.builtin.patchType] : "OPLL"},
                {"patch_no", p.builtin.patchNo}
            }}
        };
    } else {
        json ops = json::array();
        int n = std::clamp(operatorCountForVoicePatchType(voicePatchType), 1, MAX_HW_OPS);
        for (int i = 0; i < n; ++i) ops.push_back(hwOpToJson(p.hwOp[i]));
        out = json{
            {"id",p.id},{"name",p.name},
            {"FB",p.hw.FB},{"ALG",p.hw.ALG},{"AMS",p.hw.AMS},{"PMS",p.hw.PMS},{"NFQ",p.hw.NFQ},
            {"FB2",p.hw.FB2},
            {"ops",ops},
            {"ext",json{{"DM0",p.ext.DM0},
                        {"ALG_EXT",p.ext.ALG_EXT},{"HWEP",p.ext.HWEP},
                        {"target_voice_patch_type",p.ext.targetVoicePatchType},
                        {"rhythm_ch",p.ext.rhythmCh}}}
        };
    }
    // sw_bank/sw_prog は -1(参照なし)がデフォルトのため、設定されている
    // 場合のみ出力する(既存ファイルとの差分を最小化する)。
    if (p.swBank >= 0) out["sw_bank"] = p.swBank;
    if (p.swProg >= 0) out["sw_prog"] = p.swProg;
    return out;
}
HwPatch jsonToHwPatch(const json& j, uint32_t bank, uint32_t prog) {
    HwPatch p;
    p.id = (bank << 16) | prog;
    if (j.contains("name")) {
        std::string n = j["name"].get<std::string>();
        std::strncpy(p.name, n.c_str(), sizeof(p.name)-1);
    }
    if (j.contains("sw_bank")) p.swBank = static_cast<int8_t>(j["sw_bank"].get<int>());
    if (j.contains("sw_prog")) p.swProg = static_cast<int8_t>(j["sw_prog"].get<int>());
    if (j.contains("builtin")) {
        // builtin参照専用エントリ: ops[]/ext等は読まない(排他、oneOfで
        // スキーマレベルでも強制済み)。
        static const std::unordered_map<std::string, int8_t> kBuiltinTypeMap = {
            {"OPLL", 0}, {"OPLLX", 1}, {"OPLLP", 2}, {"VRC7", 3}
        };
        const auto& bj = j["builtin"];
        if (bj.contains("patch_type")) {
            auto it = kBuiltinTypeMap.find(bj["patch_type"].get<std::string>());
            if (it != kBuiltinTypeMap.end()) p.builtin.patchType = it->second;
        }
        if (bj.contains("patch_no")) p.builtin.patchNo = static_cast<int8_t>(bj["patch_no"].get<int>());
        return p;
    }
    auto g8 = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g8("FB",p.hw.FB); g8("ALG",p.hw.ALG); g8("AMS",p.hw.AMS);
    g8("PMS",p.hw.PMS); g8("NFQ",p.hw.NFQ); g8("FB2",p.hw.FB2);
    if (j.contains("ops") && j["ops"].is_array()) {
        for (int i = 0; i < MAX_HW_OPS && i < (int)j["ops"].size(); ++i)
            jsonToHwOp(j["ops"][i], p.hwOp[i]);
    }
    if (j.contains("ext")) {
        // 注意: ext サブオブジェクトのフィールドは ex から読む必要がある。
        // (以前は誤って親スコープ j を参照する g8 を流用しており、
        //  ext.* が正しく読み込まれていなかった)
        const auto& ex = j["ext"];
        auto ge8 = [&](const char* k, uint8_t& v){ if(ex.contains(k)) v=ex[k].get<uint8_t>(); };
        ge8("DM0",p.ext.DM0); ge8("ALG_EXT",p.ext.ALG_EXT);
        ge8("target_voice_patch_type",p.ext.targetVoicePatchType);
        ge8("rhythm_ch",p.ext.rhythmCh);
        if (ex.contains("HWEP")) p.ext.HWEP = ex["HWEP"].get<uint16_t>();
    }
    return p;
}

// SysExによるHwPatchパラメータオーバーライド用のマージ実装
// (2026年7月新設)。jsonToHwPatch()と異なり、まっさらなHwPatchからでは
// なく既存のtargetへ差分マージする(存在するキーのみ上書き)。
// id/name/builtinはこの経路では対象外(意図的に無視する。builtinの
// 有無で分岐すると「音色データが標準のHwPatch形状かどうか」の判断が
// 必要になり複雑化するため、そもそも対象にしない)。
// "ops"はnull/{}の要素をスキップとして扱う点がjsonToHwPatch()との
// 主な違い(全要素を常に上書きするjsonToHwPatch()に対し、こちらは
// 明示的に指定された要素だけを上書きする)。
static void mergeHwPatchFromJson(const json& j, HwPatch& target) {
    auto g8 = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g8("FB",target.hw.FB); g8("ALG",target.hw.ALG); g8("AMS",target.hw.AMS);
    g8("PMS",target.hw.PMS); g8("NFQ",target.hw.NFQ); g8("FB2",target.hw.FB2);
    if (j.contains("ops") && j["ops"].is_array()) {
        const auto& opsArr = j["ops"];
        for (int i = 0; i < MAX_HW_OPS && i < static_cast<int>(opsArr.size()); ++i) {
            const auto& oj = opsArr[i];
            if (oj.is_null()) continue;                     // null = スキップ
            if (oj.is_object() && oj.empty()) continue;      // {} = スキップ
            jsonToHwOp(oj, target.hwOp[i]);   // 既存のtarget.hwOp[i]へマージ
        }
    }
    if (j.contains("ext")) {
        const auto& ex = j["ext"];
        auto ge8 = [&](const char* k, uint8_t& v){ if(ex.contains(k)) v=ex[k].get<uint8_t>(); };
        ge8("DM0",target.ext.DM0); ge8("ALG_EXT",target.ext.ALG_EXT);
        ge8("target_voice_patch_type",target.ext.targetVoicePatchType);
        ge8("rhythm_ch",target.ext.rhythmCh);
        if (ex.contains("HWEP")) target.ext.HWEP = ex["HWEP"].get<uint16_t>();
    }
    if (j.contains("sw_bank")) target.swBank = static_cast<int8_t>(j["sw_bank"].get<int>());
    if (j.contains("sw_prog")) target.swProg = static_cast<int8_t>(j["sw_prog"].get<int>());
}

json swOpToJson(const FmSwOp& op) {
    return json{
        {"VTL",op.VTL},{"VAR",op.VAR},{"VDR",op.VDR},{"VSL",op.VSL},
        {"VSR",op.VSR},{"VRR",op.VRR},{"VLD",op.VLD},{"VLR",op.VLR},
        {"SLW",op.SLW},{"SLS",op.SLS},{"SLM",op.SLM},{"SLD",op.SLD},
        {"SLY",op.SLY},{"SLR",op.SLR},{"SLI",op.SLI}
    };
}
void jsonToSwOp(const json& j, FmSwOp& op) {
    auto g = [&](const char* k, uint8_t& v){ if(j.contains(k)) v=j[k].get<uint8_t>(); };
    g("VTL",op.VTL); g("VAR",op.VAR); g("VDR",op.VDR); g("VSL",op.VSL);
    g("VSR",op.VSR); g("VRR",op.VRR); g("VLD",op.VLD); g("VLR",op.VLR);
    g("SLW",op.SLW); g("SLS",op.SLS); g("SLM",op.SLM); g("SLD",op.SLD);
    g("SLY",op.SLY); g("SLR",op.SLR); g("SLI",op.SLI);
}

json toneLayerToJson(const ToneLayer& l) {
    return json{
        {"voice_patch_type",l.voicePatchType},
        {"hw_bank",l.hwBank},{"hw_prog",l.hwProg},
        {"note_range_lo",l.noteRangeLo},{"note_range_hi",l.noteRangeHi},
        {"transpose",l.transpose},
        {"volume_offset",l.volumeOffset},{"pan_offset",l.panOffset},
        {"enabled",l.enabled}
    };
}
ToneLayer jsonToToneLayer(const json& j) {
    ToneLayer l;
    if(j.contains("voice_patch_type")) l.voicePatchType = j["voice_patch_type"].get<uint8_t>();
    if(j.contains("hw_bank"))      l.hwBank      = j["hw_bank"].get<uint8_t>();
    if(j.contains("hw_prog"))      l.hwProg      = j["hw_prog"].get<uint8_t>();
    if(j.contains("note_range_lo"))l.noteRangeLo = j["note_range_lo"].get<uint8_t>();
    if(j.contains("note_range_hi"))l.noteRangeHi = j["note_range_hi"].get<uint8_t>();
    if(j.contains("transpose"))    l.transpose   = j["transpose"].get<int8_t>();
    if(j.contains("volume_offset"))l.volumeOffset= j["volume_offset"].get<int8_t>();
    if(j.contains("pan_offset"))   l.panOffset   = j["pan_offset"].get<int8_t>();
    if(j.contains("enabled"))      l.enabled     = j["enabled"].get<bool>();
    return l;
}

// ────────────────────────────────────────────────────────────────
//  SampleZonePatch JSON変換 (VOICE_PATCH_AWM等、サンプルベース音源系)
//  HwPatchとは完全に独立したスキーマ。vel_min/vel_max/root_noteは
//  省略可能 (省略時はSampleZoneのデフォルト値=ベロシティレイヤー無し・
//  A4=440Hz基準を使うため、ベロシティレイヤーを使わないシンプルな
//  キーゾーンのみのバンクは最小限の記述で書ける)。
json sampleZoneToJson(const SampleZone& z) {
    return json{
        {"key_min", z.keyMin}, {"key_max", z.keyMax},
        {"vel_min", z.velMin}, {"vel_max", z.velMax},
        {"wave_index", z.waveIndex}, {"root_note", z.rootNote}
    };
}
SampleZone jsonToSampleZone(const json& j) {
    SampleZone z;
    if (j.contains("key_min"))    z.keyMin    = j["key_min"].get<uint8_t>();
    if (j.contains("key_max"))    z.keyMax    = j["key_max"].get<uint8_t>();
    if (j.contains("vel_min"))    z.velMin    = j["vel_min"].get<uint8_t>();
    if (j.contains("vel_max"))    z.velMax    = j["vel_max"].get<uint8_t>();
    if (j.contains("wave_index")) z.waveIndex = j["wave_index"].get<uint16_t>();
    if (j.contains("root_note"))  z.rootNote  = j["root_note"].get<uint8_t>();
    return z;
}
json sampleZonePatchToJson(const SampleZonePatch& p) {
    json zones = json::array();
    for (const auto& z : p.zones) zones.push_back(sampleZoneToJson(z));
    return json{{"id", p.id}, {"name", p.name}, {"zones", zones}};
}
SampleZonePatch jsonToSampleZonePatch(const json& j, uint32_t bank, uint32_t prog) {
    SampleZonePatch p;
    p.id = (bank << 16) | prog;
    if (j.contains("name")) {
        std::string n = j["name"].get<std::string>();
        std::strncpy(p.name, n.c_str(), sizeof(p.name) - 1);
    }
    if (j.contains("zones") && j["zones"].is_array()) {
        for (const auto& zj : j["zones"]) p.zones.push_back(jsonToSampleZone(zj));
    }
    return p;
}

} // anonymous namespace

// SysExによるHwPatchパラメータオーバーライド用。詳細はヘッダの
// 宣言コメント参照。実際のフィールドマージは上のmergeHwPatchFromJson()
// (このファイル内、匿名名前空間の自由関数)に委譲する。
bool PatchManager::mergeHwPatchFromJsonText(const std::string& jsonText, HwPatch& target,
                                             std::string* errorOut) const
{
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = std::string("JSON parse error: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (errorOut) *errorOut = "JSON root must be an object";
        return false;
    }
    mergeHwPatchFromJson(j, target);
    return true;
}

// ================================================================
//  HwBank JSON I/O
// ================================================================

bool PatchManager::loadHwBankJson(const std::filesystem::path& path,
                                   HwBankRegistry::VoiceGroup group, int bankNo,
                                   uint8_t voicePatchType)
{
    reportProgress("Loading HwBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = hwReg_.getOrCreate(group, bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        bank.voicePatchType = voicePatchType;
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                bank.set(prog, jsonToHwPatch(entry, bankNo, prog));
            }
        }
        FITOM_LOG_INFO("HwBank loaded: " << bank.name
            << " (" << path.filename().string() << ")"
            << " voicePatchType=0x" << std::hex << (int)voicePatchType);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("HwBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::loadOpllBuiltinMetaBankJson(const std::filesystem::path& path)
{
    reportProgress("Loading OPLL builtin swPatch meta bank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        opllBuiltinMetaBank_ = HwBank{};
        if (j.contains("name")) opllBuiltinMetaBank_.name = j["name"].get<std::string>();
        opllBuiltinMetaBank_.filename = path.string();
        // このバンクのbank/prog番号自体は検索キーとして使われない
        // (findByBuiltinRef()がbuiltinフィールドで線形探索するため)。
        // idの一意性さえ保てればよいので、prog自体をそのままbankNoの
        // 代わりに使う。
        if (j.contains("patches") && j["patches"].is_array()) {
            int autoProg = 0;
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", autoProg);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                opllBuiltinMetaBank_.set(prog, jsonToHwPatch(entry, 0, prog));
                autoProg = prog + 1;
            }
        }
        hasOpllBuiltinMetaBank_ = true;
        FITOM_LOG_INFO("OPLL builtin swPatch meta bank loaded: " << opllBuiltinMetaBank_.name
            << " (" << path.filename().string() << ")");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("OPLL builtin swPatch meta bank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveHwBankJson(const std::filesystem::path& path,
                                   HwBankRegistry::VoiceGroup group, int bankNo) const
{
    const HwBank* bank = hwReg_.find(group, bankNo);
    if (!bank) return false;
    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        auto pj = hwPatchToJson(p, bank->voicePatchType);
        pj["prog"] = i;
        patches.push_back(pj);
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream f(path);
    if (!f) return false;
    f << out.dump(2);
    return true;
}

// ================================================================
//  SampleZoneBank JSON I/O (VOICE_PATCH_AWM等、サンプルベース音源系)
//  HwBankRegistryとは完全に独立したロード/セーブ経路。
// ================================================================

bool PatchManager::loadSampleZoneBankJson(const std::filesystem::path& path,
                                           int bankNo, uint8_t voicePatchType)
{
    reportProgress("Loading SampleZoneBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = sampleReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        bank.voicePatchType = voicePatchType;
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                bank.set(prog, jsonToSampleZonePatch(entry, bankNo, prog));
            }
        }
        FITOM_LOG_INFO("SampleZoneBank loaded: " << bank.name
            << " (" << path.filename().string() << ")"
            << " voicePatchType=0x" << std::hex << (int)voicePatchType);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("SampleZoneBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveSampleZoneBankJson(const std::filesystem::path& path, int bankNo) const
{
    const SampleZoneBank* bank = sampleReg_.find(bankNo);
    if (!bank) return false;
    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        auto pj = sampleZonePatchToJson(p);
        pj["prog"] = i;
        patches.push_back(pj);
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream f(path);
    if (!f) return false;
    f << out.dump(2);
    return true;
}

// ================================================================
//  SwBank JSON I/O
// ================================================================

bool PatchManager::loadSwBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading SwBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = swReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                SwPatch p;
                p.id = (uint32_t(bankNo) << 16) | prog;
                if (entry.contains("name")) {
                    std::string n = entry["name"].get<std::string>();
                    std::strncpy(p.name, n.c_str(), sizeof(p.name)-1);
                }
                if (entry.contains("sw")) {
                    const auto& sw = entry["sw"];
                    auto g8 = [&](const char* k, uint8_t& v){
                        if(sw.contains(k)) v=sw[k].get<uint8_t>();
                    };
                    g8("LWF",p.sw.LWF); g8("LFS",p.sw.LFS); g8("LFM",p.sw.LFM);
                    g8("LFD",p.sw.LFD); g8("LFR",p.sw.LFR); g8("LFI",p.sw.LFI);
                    if (sw.contains("depth_cents"))
                        p.sw.depthCents = static_cast<int16_t>(sw["depth_cents"].get<int>());
                }
                if (entry.contains("ops") && entry["ops"].is_array()) {
                    for (int i = 0; i < MAX_HW_OPS && i < (int)entry["ops"].size(); ++i)
                        jsonToSwOp(entry["ops"][i], p.swOp[i]);
                }
                if (entry.contains("fine_transpose"))
                    p.fineTranspose = static_cast<int16_t>(entry["fine_transpose"].get<int>());
                bank.set(prog, p);
            }
        }
        FITOM_LOG_INFO("SwBank loaded: " << bank.name);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("SwBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveSwBankJson(const std::filesystem::path& path, int bankNo) const
{
    const SwBank* bank = swReg_.find(bankNo);
    if (!bank) return false;

    auto swOpToJson = [](const FmSwOp& op) {
        return json{
            {"VTL",op.VTL},{"VAR",op.VAR},{"VDR",op.VDR},{"VSL",op.VSL},
            {"VSR",op.VSR},{"VRR",op.VRR},{"VLD",op.VLD},{"VLR",op.VLR},
            {"SLW",op.SLW},{"SLS",op.SLS},{"SLM",op.SLM},{"SLD",op.SLD},
            {"SLY",op.SLY},{"SLR",op.SLR},{"SLI",op.SLI}
        };
    };

    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        json ops = json::array();
        for (int op = 0; op < MAX_HW_OPS; ++op) ops.push_back(swOpToJson(p.swOp[op]));
        patches.push_back(json{
            {"prog", i}, {"name", p.name},
            {"sw", json{
                {"LWF",p.sw.LWF},{"LFS",p.sw.LFS},{"LFM",p.sw.LFM},
                {"LFD",p.sw.LFD},{"LFR",p.sw.LFR},{"LFI",p.sw.LFI},
                {"depth_cents",p.sw.depthCents}
            }},
            {"ops", ops},
            {"fine_transpose", p.fineTranspose}
        });
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    FITOM_LOG_INFO("SwBank saved: " << path.string());
    return true;
}

// ================================================================
//  PatchBank JSON I/O
// ================================================================

bool PatchManager::loadPatchBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading PatchBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = getPatchBank(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();
        if (j.contains("patches") && j["patches"].is_array()) {
            for (auto& entry : j["patches"]) {
                int prog = entry.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;
                Patch p;
                p.id = (uint32_t(bankNo) << 16) | prog;
                if (entry.contains("name")) {
                    std::string n = entry["name"].get<std::string>();
                    std::strncpy(p.name, n.c_str(), sizeof(p.name)-1);
                }
                if (entry.contains("poly"))    p.poly   = entry["poly"].get<uint8_t>();
                if (entry.contains("layers") && entry["layers"].is_array()) {
                    int li = 0;
                    for (auto& lj : entry["layers"]) {
                        if (li >= MAX_TONE_LAYERS) break;
                        p.layers[li++] = jsonToToneLayer(lj);
                    }
                }
                bank.set(prog, p);
            }
        }
        FITOM_LOG_INFO("PatchBank loaded: " << bank.name);
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("PatchBank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::savePatchBankJson(const std::filesystem::path& path, int bankNo) const
{
    const PatchBank* bank = findPatchBank(bankNo);
    if (!bank) return false;
    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& p = bank->get(i);
        if (!p.isValid()) continue;
        json layers = json::array();
        for (const auto& l : p.layers) {
            if (l.isActive()) layers.push_back(toneLayerToJson(l));
        }
        patches.push_back(json{
            {"prog",i},{"name",p.name},
            {"poly",p.poly},{"layers",layers}
        });
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream f(path);
    if (!f) return false;
    f << out.dump(2);
    return true;
}

// ================================================================
//  旧 INI 形式の互換ロード
//  FITOMCfg.cpp の ParseVoiceBank 相当
// ================================================================
bool PatchManager::loadHwBankLegacy(const std::filesystem::path& path,
                                     HwBankRegistry::VoiceGroup group, int bankNo)
{
    // 既存 CFITOMConfig::ParseVoiceBank で読んだ CFMBank を
    // HwBankRegistry に変換するブリッジ。
    // フェーズ6 でチップドライバ移行が完了したら本実装に差し替える。
    FITOM_LOG_WARN("loadHwBankLegacy: " << path.string()
        << " — legacy INI loader not yet fully implemented; use JSON format");
    return false;
}

// ================================================================
//  DrumBank JSON I/O
// ================================================================

bool PatchManager::loadDrumBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading DrumBank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = drumReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();

        if (j.contains("patches") && j["patches"].is_array()) {
            for (const auto& pj : j["patches"]) {
                int prog = pj.value("prog", -1);
                if (prog < 0 || prog >= BANK_PROG_SIZE) continue;

                DrumPatch dp;
                dp.id = (uint32_t(bankNo) << 16) | prog;
                if (pj.contains("name")) {
                    std::string n = pj["name"].get<std::string>();
                    std::strncpy(dp.name, n.c_str(), sizeof(dp.name) - 1);
                }

                if (pj.contains("notes") && pj["notes"].is_array()) {
                    for (const auto& nj : pj["notes"]) {
                        int noteNo = nj.value("note", -1);
                        if (noteNo < 0 || noteNo >= 128) continue;

                        DrumNote& dn = dp.notes[noteNo];
                        dn.enabled    = true;
                        dn.patchBank  = nj.value("patch_bank", static_cast<uint8_t>(0));
                        dn.patchProg  = nj.value("patch_prog", static_cast<uint8_t>(0));
                        dn.playNote   = nj.value("play_note",  static_cast<uint8_t>(60));
                        dn.fineTune   = nj.value("fine_tune",  static_cast<int16_t>(0));
                        dn.pan        = static_cast<int8_t>(nj.value("pan", 0));
                        dn.gateTime   = nj.value("gate_time",  static_cast<uint16_t>(0));
                        if (nj.contains("name")) {
                            std::string nn = nj["name"].get<std::string>();
                            std::strncpy(dn.name, nn.c_str(), sizeof(dn.name) - 1);
                        }
                    }
                }
                bank.set(prog, dp);
            }
        }
        FITOM_LOG_INFO("DrumBank loaded: " << bank.name
            << " (" << path.filename().string() << ")");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("DrumBank parse error: " << e.what());
        return false;
    }
}

// ================================================================
//  DrumKit JSON I/O (新方式: prog単位の独立ファイル)
//
//  "type"フィールドで2種類のキット定義を判別する:
//    "routed" (省略時のデフォルト、後方互換): ノートごとに任意の
//      Patch(bank/prog)へ個別にルーティングする、旧来のnotes[]配列形式。
//      FM合成系チップのように、ドラム音1つ1つが別々のPatchとして
//      定義されている場合に使う。
//    "direct": チップ自身が内蔵のキーゾーン切り替えを持つ場合
//      (OPL4 AWM等) の圧縮表現。単一のPatch(bank/prog)への
//      パススルーを [note_min, note_max] の範囲に自動展開する
//      (playNote = 受信ノート番号そのまま)。ノート数分の重複記述を
//      書かずに済む。ロード時にDrumNote配列へ展開するため、
//      CRhythmCh側のランタイムコードは一切変更不要。
// ================================================================

bool PatchManager::loadDrumKitJson(const std::filesystem::path& path, int prog)
{
    if (prog < 0 || prog >= BANK_PROG_SIZE) {
        FITOM_LOG_ERR("loadDrumKitJson: invalid prog=" << prog);
        return false;
    }
    reportProgress("Loading DrumKit: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }

    try {
        json j = json::parse(f, nullptr, true, true);
        std::string type = j.value("type", "routed");

        DrumPatch dp;
        dp.id = (uint32_t(0) << 16) | prog; // ドラムバンクは常に固定バンク番号0
        if (j.contains("name")) {
            std::string n = j["name"].get<std::string>();
            std::strncpy(dp.name, n.c_str(), sizeof(dp.name) - 1);
        }

        if (type == "direct") {
            // 単一Patchへのパススルーを [note_min, note_max] に自動展開
            uint8_t voicePatchType = j.value("voice_patch_type", static_cast<uint8_t>(VOICE_PATCH_NONE));
            uint8_t patchBank = j.value("patch_bank", static_cast<uint8_t>(0));
            uint8_t patchProg = j.value("patch_prog", static_cast<uint8_t>(0));
            int noteMin = j.value("note_min", 0);
            int noteMax = j.value("note_max", 127);
            int8_t  swBank   = static_cast<int8_t>(j.value("sw_bank", -1));
            int8_t  swProg   = static_cast<int8_t>(j.value("sw_prog", -1));
            int8_t  pan      = static_cast<int8_t>(j.value("pan", 0));
            uint16_t gateTime = j.value("gate_time", static_cast<uint16_t>(0));
            int16_t fineTune  = j.value("fine_tune", static_cast<int16_t>(0));

            if (noteMin < 0 || noteMax > 127 || noteMin > noteMax) {
                FITOM_LOG_ERR("DrumKit(direct) invalid note range ["
                    << noteMin << "," << noteMax << "]: " << path.string());
                return false;
            }
            for (int n = noteMin; n <= noteMax; ++n) {
                DrumNote& dn = dp.notes[n];
                dn.enabled   = true;
                dn.voicePatchType = voicePatchType;
                dn.patchBank = patchBank;
                dn.patchProg = patchProg;
                dn.playNote  = static_cast<uint8_t>(n); // 受信ノートをそのまま渡す
                dn.fineTune  = fineTune;
                dn.swBank    = swBank;
                dn.swProg    = swProg;
                dn.pan       = pan;
                dn.gateTime  = gateTime;
            }
        } else if (type == "routed") {
            // 旧来のnotes[]配列形式 (ノートごとに個別のPatchへルーティング)
            if (j.contains("notes") && j["notes"].is_array()) {
                for (const auto& nj : j["notes"]) {
                    int noteNo = nj.value("note", -1);
                    if (noteNo < 0 || noteNo >= 128) continue;

                    DrumNote& dn = dp.notes[noteNo];
                    dn.enabled    = true;
                    dn.voicePatchType = nj.value("voice_patch_type", static_cast<uint8_t>(VOICE_PATCH_NONE));
                    dn.patchBank  = nj.value("patch_bank", static_cast<uint8_t>(0));
                    dn.patchProg  = nj.value("patch_prog", static_cast<uint8_t>(0));
                    dn.playNote   = nj.value("play_note",  static_cast<uint8_t>(60));
                    dn.fineTune   = nj.value("fine_tune",  static_cast<int16_t>(0));
                    dn.swBank     = static_cast<int8_t>(nj.value("sw_bank", -1));
                    dn.swProg     = static_cast<int8_t>(nj.value("sw_prog", -1));
                    dn.pan        = static_cast<int8_t>(nj.value("pan", 0));
                    dn.gateTime   = nj.value("gate_time",  static_cast<uint16_t>(0));
                    if (nj.contains("name")) {
                        std::string nn = nj["name"].get<std::string>();
                        std::strncpy(dn.name, nn.c_str(), sizeof(dn.name) - 1);
                    }
                }
            }

            // チョークグループ: 同一グループ内のノートが相互にダンプし
            // 合う(ハイヒハット等)。1ノートが複数グループへ二重登録
            // されるのは曖昧なため、検出したら警告して以降を無視する
            // (最初に見つかったグループのみ採用)。
            if (j.contains("choke_groups") && j["choke_groups"].is_array()) {
                std::array<bool, 128> assigned{};
                for (const auto& gj : j["choke_groups"]) {
                    if (!gj.is_array()) continue;
                    std::vector<uint8_t> group;
                    for (const auto& nv : gj) {
                        int n = nv.get<int>();
                        if (n < 0 || n >= 128) {
                            FITOM_LOG_WARN("DrumKit: choke_groups note=" << n
                                << " out of range, ignored (" << path.string() << ")");
                            continue;
                        }
                        if (assigned[static_cast<size_t>(n)]) {
                            FITOM_LOG_WARN("DrumKit: note=" << n
                                << " already belongs to another choke_group, ignored ("
                                << path.string() << ")");
                            continue;
                        }
                        assigned[static_cast<size_t>(n)] = true;
                        group.push_back(static_cast<uint8_t>(n));
                    }
                    if (group.size() >= 2) {
                        dp.chokeGroups.push_back(std::move(group));
                    } else if (!group.empty()) {
                        FITOM_LOG_WARN("DrumKit: choke_group with <2 notes ignored ("
                            << path.string() << ")");
                    }
                }
            }
        } else {
            FITOM_LOG_ERR("DrumKit: unknown type '" << type << "': " << path.string());
            return false;
        }

        auto& bank = drumReg_.getOrCreate(0); // ドラムバンクは常に固定バンク番号0
        bank.filename = path.string();
        bank.set(prog, dp);

        FITOM_LOG_INFO("DrumKit loaded: prog=" << prog << " '" << dp.name
            << "' type=" << type << " (" << path.filename().string() << ")");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("DrumKit parse error: " << e.what() << " (" << path.string() << ")");
        return false;
    }
}

bool PatchManager::saveDrumBankJson(const std::filesystem::path& path, int bankNo) const
{
    const DrumPatchBank* bank = nullptr;
    // DrumBankRegistry に find が必要 — getOrCreate の const 版を追加する方針で
    // ここでは drumReg_ を mutable にして対応
    auto& reg = const_cast<DrumBankRegistry&>(drumReg_);
    if (!reg.hasBank(bankNo)) return false;
    bank = &reg.getOrCreate(bankNo); // 存在確認済み

    json patches = json::array();
    for (int i = 0; i < BANK_PROG_SIZE; ++i) {
        const auto& dp = bank->get(i);
        if (!dp.isValid()) continue;

        json notes = json::array();
        for (int n = 0; n < 128; ++n) {
            const auto& dn = dp.notes[n];
            if (!dn.isActive()) continue;
            notes.push_back(json{
                {"note",       n},
                {"name",       dn.name},
                {"patch_bank", dn.patchBank},
                {"patch_prog", dn.patchProg},
                {"play_note",  dn.playNote},
                {"fine_tune",  dn.fineTune},
                {"sw_bank",    dn.swBank},
                {"sw_prog",    dn.swProg},
                {"pan",        dn.pan},
                {"gate_time",  dn.gateTime}
            });
        }
        patches.push_back(json{
            {"prog",  i},
            {"name",  dp.name},
            {"notes", notes}
        });
    }
    json out = {{"name", bank->name}, {"patches", patches}};
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    FITOM_LOG_INFO("DrumBank saved: " << path.string());
    return true;
}

// 旧 INI 形式のドラムバンク読み込み (移行期互換)
bool PatchManager::loadDrumBankLegacy(const std::filesystem::path& path, int bankNo)
{
    // 旧フォーマット:
    //   [Header]
    //   Type=RHYTHM
    //   BankName=Standard Kit
    //   [Bank]
    //   Note35=Bass Drum,OPN,0,0,24,0,0,-1
    //   ; フォーマット: name, DevName, BankNo, ProgNo, NoteNum:FineTune, Pan, GateTime, Ch
    reportProgress("Loading DrumBank (legacy): " + path.string());

    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }

    auto& bank = drumReg_.getOrCreate(bankNo);
    bank.filename = path.string();

    // INI パーサー (Config.cpp の ini::parse 相当)
    DrumPatch dp;
    dp.id = (uint32_t(bankNo) << 16) | 0; // prog=0 固定 (旧仕様)

    std::string section, line;
    while (std::getline(f, line)) {
        // BOM 除去・trim
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF)
            line = line.substr(3);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (section == "Header") {
            if (key == "BankName") {
                bank.name = val;
                std::strncpy(dp.name, val.c_str(), sizeof(dp.name) - 1);
            }
            continue;
        }
        if (section == "Bank" && key.substr(0, 4) == "Note") {
            int noteNo = std::stoi(key.substr(4));
            if (noteNo < 0 || noteNo >= 128) continue;

            // カンマ分割: name,DevName,BankNo,ProgNo,Note[:FineTune],Pan,Gate[,Ch]
            // 末尾の[,Ch]列(旧チャンネル固定値)はfixed_ch廃止に伴い読み捨てる。
            // ハイハット等のチョークが必要な場合は、変換後にDrumPatch::
            // chokeGroupsで明示的に再設定すること。
            std::vector<std::string> parts;
            std::stringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) parts.push_back(tok);
            if (parts.size() < 7) continue;

            DrumNote& dn = dp.notes[noteNo];
            dn.enabled   = true;
            std::strncpy(dn.name, parts[0].c_str(), sizeof(dn.name) - 1);
            // 旧 INI の bank/prog は HwPatch 参照だが、
            // 変換ツールで対応する Patch を作成した上で patchBank/patchProg を設定する。
            // ここでは暫定的に patchBank=bank, patchProg=prog とする。
            dn.patchBank = static_cast<uint8_t>(std::stoi(parts[2]));
            dn.patchProg = static_cast<uint8_t>(std::stoi(parts[3]));
            // Note[:FineTune] の分解
            auto colon = parts[4].find(':');
            if (colon != std::string::npos) {
                dn.playNote = static_cast<uint8_t>(std::stoi(parts[4].substr(0, colon)));
                dn.fineTune = static_cast<int16_t>(std::stoi(parts[4].substr(colon + 1)));
            } else if (!parts[4].empty() && parts[4][0] == '#') {
                // #XXXX = raw fnum
                dn.playNote = 255;
                dn.fineTune = static_cast<int16_t>(std::stoi(parts[4].substr(1), nullptr, 16));
            } else {
                dn.playNote = static_cast<uint8_t>(std::stoi(parts[4]));
            }
            dn.pan      = static_cast<int8_t>(std::stoi(parts[5]));
            dn.gateTime = static_cast<uint16_t>(std::stoi(parts[6]));

            FITOM_LOG_DEBUG("DrumBank legacy: note=" << noteNo
                << " '" << dn.name << "' patchBank=" << (int)dn.patchBank
                << " patchProg=" << (int)dn.patchProg);
        }
    }
    bank.set(0, dp);
    FITOM_LOG_INFO("DrumBank (legacy) loaded: " << bank.name);
    return true;
}

// ================================================================
//  SCC Wave Bank JSON I/O
// ================================================================
#include "fitom/SccWaveData.h"
#include <cmath>
#include <sstream>

bool PatchManager::loadSccWaveBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading SCC Wave Bank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }
    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = sccWaveReg_.getOrCreate(bankNo);
        if (j.contains("name")) bank.name = j["name"].get<std::string>();
        bank.filename = path.string();

        if (j.contains("waves") && j["waves"].is_array()) {
            for (const auto& wj : j["waves"]) {
                int waveNo = wj.value("wave_no", -1);
                if (waveNo < 0 || waveNo >= SCC_MAX_WAVES) continue;

                SccWave wave;
                wave.waveNo = static_cast<uint8_t>(waveNo);
                if (wj.contains("name")) {
                    std::string n = wj["name"].get<std::string>();
                    std::strncpy(wave.name, n.c_str(), sizeof(wave.name) - 1);
                }

                const auto& data = wj["data"];
                if (data.is_array()) {
                    // 配列形式: [100, -100, ...]
                    for (int i = 0; i < SCC_WAVE_SIZE && i < (int)data.size(); ++i) {
                        wave.data[i] = static_cast<int8_t>(
                            std::clamp(data[i].get<int>(), -128, 127));
                    }
                } else if (data.is_string()) {
                    // 16進文字列形式: "64AB..."
                    std::string hex = data.get<std::string>();
                    for (int i = 0; i < SCC_WAVE_SIZE && i * 2 + 1 < (int)hex.size(); ++i) {
                        uint8_t byte = static_cast<uint8_t>(
                            std::stoi(hex.substr(i * 2, 2), nullptr, 16));
                        wave.data[i] = static_cast<int8_t>(byte);
                    }
                }
                bank.setWave(static_cast<uint8_t>(waveNo), wave);
            }
        }
        FITOM_LOG_INFO("SCC Wave Bank loaded: " << bank.name
            << " (" << path.filename().string() << ")");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("SCC Wave Bank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::saveSccWaveBankJson(const std::filesystem::path& path, int bankNo) const
{
    if (!sccWaveReg_.hasBank(bankNo)) return false;
    auto& reg  = const_cast<SccWaveRegistry&>(sccWaveReg_);
    auto& bank = reg.getOrCreate(bankNo);

    json waves = json::array();
    for (int i = 0; i < SCC_MAX_WAVES; ++i) {
        const auto& wave = bank.getWave(static_cast<uint8_t>(i));
        // 全 0 は省略
        bool allZero = true;
        for (int8_t b : wave.data) if (b != 0) { allZero = false; break; }
        if (allZero && i > 1) continue;

        json arr = json::array();
        for (int8_t b : wave.data) arr.push_back(static_cast<int>(b));
        waves.push_back(json{
            {"wave_no", i},
            {"name",    wave.name},
            {"data",    arr}
        });
    }
    json out = {{"name", bank.name}, {"waves", waves}};
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    return true;
}

// ================================================================
//  PCM Bank JSON I/O (PcmBankData.h)
// ================================================================
#include "fitom/PcmBankData.h"

// PcmBank::loadBinary の実装 (ヘッダに宣言、cpp に実体)
bool PcmBank::loadBinary(const std::filesystem::path& basePath)
{
    std::filesystem::path binPath = this->binPath;
    if (binPath.is_relative() && !basePath.empty())
        binPath = basePath / binPath;

    std::ifstream f(binPath, std::ios::binary);
    if (!f) {
        FITOM_LOG_ERR("PcmBank: cannot open bin: " << binPath.string());
        return false;
    }
    binData.assign(std::istreambuf_iterator<char>(f),
                   std::istreambuf_iterator<char>());
    FITOM_LOG_INFO("PcmBank: loaded " << binData.size()
        << " bytes from " << binPath.string());
    return true;
}

bool PatchManager::loadPcmBankJson(const std::filesystem::path& path, int bankNo)
{
    reportProgress("Loading PCM Bank: " + path.string());
    std::ifstream f(path);
    if (!f) { FITOM_LOG_ERR("Cannot open: " << path.string()); return false; }

    try {
        json j = json::parse(f, nullptr, true, true);
        auto& bank = pcmReg_.getOrCreate(bankNo);
        bank.name       = j.value("name",   "");
        bank.codec      = j.value("codec",  "adpcm-b");
        bank.sampleRate = j.value("sample_rate", 0u);
        bank.boundary   = j.value("boundary",    256u);
        bank.binPath    = j.value("bin_file",    "");

        const std::filesystem::path baseDir = path.parent_path();

        // adpcm_json フィールドがあれば adpcm_packer 出力 JSON を参照
        if (j.contains("adpcm_json") && j["adpcm_json"].is_string()) {
            std::filesystem::path ajPath = j["adpcm_json"].get<std::string>();
            if (ajPath.is_relative()) ajPath = baseDir / ajPath;

            std::ifstream aj(ajPath);
            if (!aj) {
                FITOM_LOG_WARN("PCM Bank: cannot open adpcm_json: " << ajPath.string()
                    << " — falling back to entries[]");
            } else {
                json aj_j = json::parse(aj, nullptr, true, true);
                // adpcm_packer の出力 JSON から entries を構築
                // entry_no は出現順で 0 から割り当てる
                if (aj_j.contains("entries") && aj_j["entries"].is_array()) {
                    uint8_t entryNo = 0;
                    for (const auto& ej : aj_j["entries"]) {
                        if (entryNo >= PCM_MAX_ENTRIES) break;
                        PcmEntry e;
                        e.entryNo    = entryNo;
                        std::string n = ej.value("name", "");
                        std::strncpy(e.name, n.c_str(), sizeof(e.name) - 1);
                        e.startOffset = ej.value("offset",      0u);
                        uint32_t paddedSize = ej.value("padded_size", 0u);
                        e.size        = ej.value("size",         0u);
                        e.paddedSize  = paddedSize;
                        e.endOffset   = e.startOffset + paddedSize - 1;
                        bank.setEntry(entryNo, e);
                        FITOM_LOG_DEBUG("PCM entry[" << (int)entryNo
                            << "]: '" << e.name
                            << "' offset=0x" << std::hex << e.startOffset
                            << " size=" << std::dec << e.paddedSize);
                        ++entryNo;
                    }
                }
                // codec / sample_rate は adpcm_json から上書きする (一致確認)
                if (aj_j.contains("codec")) {
                    std::string ac = aj_j["codec"].get<std::string>();
                    if (ac != bank.codec) {
                        FITOM_LOG_WARN("PCM Bank: codec mismatch: pcmbank='"
                            << bank.codec << "' adpcm_json='" << ac << "'");
                    }
                }
                if (aj_j.contains("sample_rate") && bank.sampleRate == 0) {
                    bank.sampleRate = aj_j["sample_rate"].get<uint32_t>();
                }
            }
        }

        // entries[] が直接記述されている場合はそちらを使う (adpcm_json より優先)
        if (j.contains("entries") && j["entries"].is_array()) {
            for (const auto& ej : j["entries"]) {
                int no = ej.value("entry_no", -1);
                if (no < 0 || no >= PCM_MAX_ENTRIES) continue;
                PcmEntry e;
                e.entryNo     = static_cast<uint8_t>(no);
                std::string n = ej.value("name", "");
                std::strncpy(e.name, n.c_str(), sizeof(e.name) - 1);
                e.startOffset = ej.value("start_offset", 0u);
                e.endOffset   = ej.value("end_offset",   0u);
                e.size        = ej.value("size",         0u);
                e.paddedSize  = ej.value("padded_size",  0u);
                bank.setEntry(static_cast<uint8_t>(no), e);
            }
        }

        // バイナリを読み込む
        bank.loadBinary(baseDir);

        FITOM_LOG_INFO("PCM Bank loaded: '" << bank.name
            << "' codec=" << bank.codec
            << " sr=" << bank.sampleRate
            << " (" << bank.binData.size() << " bytes)");
        return true;
    } catch (const std::exception& e) {
        FITOM_LOG_ERR("PCM Bank parse error: " << e.what());
        return false;
    }
}

bool PatchManager::savePcmBankJson(const std::filesystem::path& path, int bankNo) const
{
    const PcmBank* bank = pcmReg_.find(bankNo);
    if (!bank) return false;

    json entries = json::array();
    for (int i = 0; i < PCM_MAX_ENTRIES; ++i) {
        const auto& e = bank->getEntry(static_cast<uint8_t>(i));
        if (!e.isValid()) continue;
        entries.push_back(json{
            {"entry_no",    i},
            {"name",        e.name},
            {"start_offset", e.startOffset},
            {"end_offset",  e.endOffset},
            {"size",        e.size},
            {"padded_size", e.paddedSize}
        });
    }
    json out = {
        {"name",        bank->name},
        {"codec",       bank->codec},
        {"sample_rate", bank->sampleRate},
        {"boundary",    bank->boundary},
        {"bin_file",    bank->binPath},
        {"entries",     entries}
    };
    std::ofstream ofs(path);
    if (!ofs) return false;
    ofs << out.dump(2);
    return true;
}

} // namespace fitom
