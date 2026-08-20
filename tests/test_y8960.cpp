// tests/test_y8960.cpp
// Y8960(MSX用サウンドカートリッジ、統合音源チップ)の拡張ブロック向け
// チップドライバの回帰テスト。
//   - COPLLEX (DEVICE_OPLLEX): 標準OPLL(COPLL)に、チャンネル別ROM
//     プリセットバンク選択レジスタ(0x40+ch)を追加しただけの拡張。
//     BANKレジスタの書き込みが ext.ALG_EXT のbit2-1から正しく導かれる
//     ことを確認する。
//   - COPL2EX (DEVICE_OPL2EX): FMコアがOPL2(YM3812)と完全に同一の
//     レジスタマップであることを確認する。
//   - DeviceFactory / FITOMConfig の配線(composite展開、
//     VoicePatchTypeマッピング、フォールバック受け入れ)を確認する。
//
// エミュレーションエンジン側の参照実装は別リポジトリ ../Y8960emu
// (ymfm::y8960opllex / ymfm::y8960opl2ex)。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include <map>
#include <memory>
#include <vector>

using namespace fitom;

namespace {

class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
    std::vector<std::pair<uint16_t, uint8_t>> history;

    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
        history.emplace_back(addr, static_cast<uint8_t>(data));
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
    int clock = 3579545;
    int getClock() override { return clock; }
};

HwPatch makeOpllUserPatch()
{
    HwPatch p{};
    p.id = 1;
    p.hw.FB = 3;
    p.hwOp[0].TL = 20;
    p.hwOp[1].TL = 40;
    return p;
}

} // namespace

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPLLEX(IPort* p, int sr, uint8_t m);
std::unique_ptr<ISoundDevice> createCOPL2EX(IPort* p, int sr, bool rhythmMode);
}

// ================================================================
//  COPLLEX: BANKレジスタ (0x40+ch)
// ================================================================

// ext.ALG_EXT のbit2-1(値0-3)がそのままBANKレジスタへ書かれること。
// bit0(プリセット選択フラグ)には影響しない。
TEST_CASE("COPLLEX writes the per-channel BANK register from ext.ALG_EXT bits 2-1",
          "[y8960][opllex]")
{
    for (uint8_t bank = 0; bank < 4; ++bank) {
        RecordingPort port;
        auto dev = createCOPLLEX(&port, 44100, 0);
        dev->init();

        HwPatch patch = makeOpllUserPatch();
        patch.ext.ALG_EXT = static_cast<uint8_t>(bank << 1); // preset flag=0, BANK=bank
        uint8_t ch = dev->allocCh(nullptr, &patch, 100);
        REQUIRE(ch != 0xFF);

        INFO("bank=" << (int)bank << " ch=" << (int)ch);
        REQUIRE(port.regs.count(static_cast<uint16_t>(0x40 + ch)) > 0);
        CHECK(port.regs.at(static_cast<uint16_t>(0x40 + ch)) == bank);
    }
}

// プリセット選択フラグ(bit0)とBANK(bit2-1)は独立して書き込まれる。
TEST_CASE("COPLLEX combines the preset flag and BANK bits independently",
          "[y8960][opllex]")
{
    RecordingPort port;
    auto dev = createCOPLLEX(&port, 44100, 0);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    patch.ext.ALG_EXT = (2u << 1) | 1u; // preset=1, BANK=2(OPLL-P)
    patch.hw.ALG = 5;                   // プリセット音色番号5
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);

    CHECK(port.regs.at(static_cast<uint16_t>(0x40 + ch)) == 2);
    // inst番号(0x30+ch上位nibble)にプリセット音色番号が入ること
    // (COPLL::updateVoiceの既存挙動)。
    CHECK(((port.regs.at(static_cast<uint16_t>(0x30 + ch)) >> 4) & 0xF) == 5);
}

// COPLLEXは標準OPLLと同じ9ch・ユーザー音色レジスタ書き込み挙動を保つ
// (COPLLの継承のみで実現しているため、既存の0x00-0x07書き込みが
// 壊れていないことを確認する)。
TEST_CASE("COPLLEX still writes standard OPLL user-voice registers", "[y8960][opllex]")
{
    RecordingPort port;
    auto dev = createCOPLLEX(&port, 44100, 0);
    dev->init();
    CHECK(dev->getChCount() == 9);
    CHECK(dev->getDeviceType() == DEVICE_OPLLEX);

    HwPatch patch = makeOpllUserPatch();
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);

    // reg 0x03: KSL(op1)<<6 | WS(op0)<<3 | WS(op1)<<4 | FB
    CHECK((port.regs.at(0x03) & 0x07) == 3); // FB
}

// ================================================================
//  COPL2EX: FMコアはOPL2(YM3812)と完全に同一
// ================================================================

TEST_CASE("COPL2EX shares the OPL2 register map for its FM core", "[y8960][opl2ex]")
{
    RecordingPort port;
    auto dev = createCOPL2EX(&port, 44100, false);
    dev->init();

    CHECK(dev->getChCount() == 9);
    CHECK(dev->getDeviceType() == DEVICE_OPL2EX);
    // Wave Select Enable (OPL2固有、COPLには無い)
    CHECK(port.regs.at(0x01) == 0x20);

    HwPatch patch{};
    patch.id = 1;
    patch.hw.FB = 5;
    patch.hw.ALG = 1;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 69, 0, true);
    dev->noteOn(ch, 100);

    // 0xC0+ch: pan(既定0x30) | FB<<1 | ALG
    CHECK(port.regs.at(static_cast<uint16_t>(0xC0 + ch)) == (0x30 | (5 << 1) | 1));
    // B0+ch: キーオンビット(bit5)が立つ
    CHECK((port.regs.at(static_cast<uint16_t>(0xB0 + ch)) & 0x20) == 0x20);
}

// ================================================================
//  DeviceFactory 配線
// ================================================================

TEST_CASE("DeviceFactory reports the Y8960 chip layout", "[y8960][factory]")
{
    CHECK(DeviceFactory::defaultChCount(DEVICE_OPLLEX) == 9);
    CHECK(DeviceFactory::defaultChCount(DEVICE_OPL2EX) == 9);
    CHECK(DeviceFactory::defaultChCount(DEVICE_ADPCMB_OPL2EX) == 1);

    RecordingPort port;
    auto opllex = DeviceFactory::create(DEVICE_OPLLEX, &port, 44100);
    REQUIRE(opllex != nullptr);
    CHECK(opllex->getChCount() == 9);
    CHECK(opllex->getDeviceType() == DEVICE_OPLLEX);

    auto opl2ex = DeviceFactory::create(DEVICE_OPL2EX, &port, 44100);
    REQUIRE(opl2ex != nullptr);
    CHECK(opl2ex->getChCount() == 9);
    CHECK(opl2ex->getDeviceType() == DEVICE_OPL2EX);

    auto adpcm = DeviceFactory::create(DEVICE_ADPCMB_OPL2EX, &port, 44100);
    REQUIRE(adpcm != nullptr);
    CHECK(adpcm->getChCount() == 1);
    CHECK(adpcm->getDeviceType() == DEVICE_ADPCMB_OPL2EX);
}

// OPLLEXはユーザー音色に限り標準OPLL系との相互フォールバックが
// できる(opllFamilyAcceptsFallback、プリセット音色は不可)。
TEST_CASE("DeviceFactory fallback treats OPLLEX as part of the OPLL family",
          "[y8960][factory][fallback]")
{
    HwPatch userPatch = makeOpllUserPatch(); // ext.ALG_EXT=0 (ユーザー音色)
    CHECK(DeviceFactory::acceptsFallback(DEVICE_OPLLEX, VOICE_PATCH_OPLL, userPatch));
    CHECK(DeviceFactory::acceptsFallback(DEVICE_OPLL, VOICE_PATCH_OPLLEX, userPatch));

    HwPatch presetPatch{};
    presetPatch.ext.ALG_EXT = 1; // プリセット選択
    presetPatch.hw.ALG = 3;
    CHECK_FALSE(DeviceFactory::acceptsFallback(DEVICE_OPLLEX, VOICE_PATCH_OPLL, presetPatch));
}

// OPLLEX固有のBANK値(ext.ALG_EXTのbit2-1、0=OPLL/1=OPLL-X/2=OPLL-P/
// 3=VRC7)がどの値でも、プリセット音色(bit0=1)は一律フォールバック
// 対象外であること。BANK値によってALG_EXTが1/3/5/7(いずれも奇数)に
// なるため、bit0だけを見る`& 1`判定がBANK拡張バンクのプリセットを
// 取りこぼさないことを回帰確認する。
TEST_CASE("OPLLEX preset patches on every BANK value are rejected for fallback",
          "[y8960][opllex][fallback]")
{
    for (uint8_t bank = 0; bank < 4; ++bank) {
        HwPatch presetPatch{};
        presetPatch.ext.ALG_EXT = static_cast<uint8_t>((bank << 1) | 1); // preset=1
        presetPatch.hw.ALG = 3;
        INFO("bank=" << (int)bank << " ALG_EXT=" << (int)presetPatch.ext.ALG_EXT);
        REQUIRE((presetPatch.ext.ALG_EXT % 2) == 1); // 奇数であることの確認(bit0=1)

        // OPLLEX自身のプリセットが他の標準OPLL系デバイスへ流れないこと
        CHECK_FALSE(DeviceFactory::acceptsFallback(DEVICE_OPLL, VOICE_PATCH_OPLLEX, presetPatch));
        CHECK_FALSE(DeviceFactory::acceptsFallback(DEVICE_OPLLX, VOICE_PATCH_OPLLEX, presetPatch));
        CHECK_FALSE(DeviceFactory::acceptsFallback(DEVICE_OPLLP, VOICE_PATCH_OPLLEX, presetPatch));
        CHECK_FALSE(DeviceFactory::acceptsFallback(DEVICE_VRC7, VOICE_PATCH_OPLLEX, presetPatch));

        // 標準OPLL系のプリセットがOPLLEXへ流れないこと(BANK側はここでは
        // 常に0のはずだが、念のため同じpresetPatch[bank!=0でも]で確認する)
        CHECK_FALSE(DeviceFactory::acceptsFallback(DEVICE_OPLLEX, VOICE_PATCH_OPLL, presetPatch));
    }

    // 対照実験: ユーザー音色(bit0=0)ならBANK値に関わらず受け入れられること
    for (uint8_t bank = 0; bank < 4; ++bank) {
        HwPatch userPatch = makeOpllUserPatch();
        userPatch.ext.ALG_EXT = static_cast<uint8_t>(bank << 1); // preset=0
        INFO("bank=" << (int)bank);
        CHECK(DeviceFactory::acceptsFallback(DEVICE_OPLLEX, VOICE_PATCH_OPLL, userPatch));
        CHECK(DeviceFactory::acceptsFallback(DEVICE_OPLL, VOICE_PATCH_OPLLEX, userPatch));
    }
}

// OPL2EXのFMコアはOPL2そのものなので、フォールバック判定もOPL2と同じ
// 関数(copl2AcceptsFallback)を共有する。
TEST_CASE("DeviceFactory fallback treats OPL2EX like OPL2 for its FM core",
          "[y8960][factory][fallback]")
{
    HwPatch patch{};
    patch.hwOp[0].WS = 0;
    patch.hwOp[1].WS = 0;
    CHECK(DeviceFactory::acceptsFallback(DEVICE_OPL2EX, VOICE_PATCH_OPL, patch));
}

// ================================================================
//  FITOMConfig 配線
// ================================================================

TEST_CASE("FITOMConfig maps Y8960 device types to the expected VoicePatchType",
          "[y8960][config]")
{
    // OPLLEXは標準OPLL系と混同しないよう専用のVoicePatchTypeを持つ
    // (BANK選択ビットがext.ALG_EXTに追加されているため)。
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_OPLLEX) == VOICE_PATCH_OPLLEX);
    // OPL2EXのFMコアはOPL2と完全に同一のためVoicePatchTypeを共有する。
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_OPL2EX) == VOICE_PATCH_OPL2);
    // 内蔵ADPCM-BはY8950と同一レジスタ配置のためVOICE_PATCH_ADPCMBを共有する。
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_ADPCMB_OPL2EX) == VOICE_PATCH_ADPCMB);

    CHECK(FITOMConfig::stringToVoicePatchType("OPLLEX") == VOICE_PATCH_OPLLEX);
}

TEST_CASE("FITOMConfig::resolveCompositeSpec expands OPLLEX like the OPLL family",
          "[y8960][config][composite]")
{
    std::vector<FITOMConfig::SubDeviceSpec> spec;

    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_OPLLEX, false, spec));
    REQUIRE(spec.size() == 1);
    CHECK(spec[0].deviceType == DEVICE_OPLLEX);
    CHECK(spec[0].rhythmCapable);

    spec.clear();
    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_OPLLEX, true, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_OPLLEX);
    // OPLLEXの内蔵リズムは標準OPLLと完全に同一のレジスタ体系のため、
    // 専用のRHYTHM deviceTypeを持たずDEVICE_OPLL_RHYを共用する。
    CHECK(spec[1].deviceType == DEVICE_OPLL_RHY);
}

TEST_CASE("FITOMConfig::resolveCompositeSpec expands OPL2EX into FM+ADPCM(+rhythm)",
          "[y8960][config][composite]")
{
    std::vector<FITOMConfig::SubDeviceSpec> spec;

    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_OPL2EX, false, spec));
    REQUIRE(spec.size() == 2);
    CHECK(spec[0].deviceType == DEVICE_OPL2EX);
    CHECK(spec[0].rhythmCapable);
    CHECK(spec[1].deviceType == DEVICE_ADPCMB_OPL2EX);

    spec.clear();
    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_OPL2EX, true, spec));
    REQUIRE(spec.size() == 3);
    CHECK(spec[0].deviceType == DEVICE_OPL2EX);
    CHECK(spec[1].deviceType == DEVICE_ADPCMB_OPL2EX);
    // リズムもOPL2と完全に同一のレジスタ体系のためDEVICE_OPL_RHYを共用する。
    CHECK(spec[2].deviceType == DEVICE_OPL_RHY);
}

// ================================================================
//  OPLL系ROM音色 (バンク0固定の暗黙HwPatch) → OPLLEXフォールバック
//
//  PatchManager::initOpllRomPatches()が生成する
//  opllRomPatches_[variantSel][instIndex] は、要求元チップ
//  (OPLL/OPLLX/OPLLP/VRC7)が未接続の場合にOPLLEXへフォールバックできる
//  よう、生成時点でext.ALG_EXTのbit2-1にvariantSelをBANK値として
//  焼き込んでおく設計になった(resolveOpllRomVoice()参照)。
//  ここではFITOMConfig(実デバイス)を介さず、PatchManagerの読み取り
//  専用アクセサ(getOpllRomPatches/getOpllRomPatchByProg)経由でこの
//  データが正しく焼き込まれていることを確認する — resolveOpllRomVoice()
//  自体のデバイス選択分岐(接続デバイス一覧が必要)はFITOMConfigに実機/
//  プラグイン無しでデバイスを注入するテスト用の仕組みが無いため、
//  このテストスイートでは直接検証できない(2026年8月時点の既知の
//  カバレッジ上限)。
// ================================================================
TEST_CASE("OPLL ROM preset patches bake their source chip's BANK value into ALG_EXT",
          "[y8960][opllex][patchmanager]")
{
    PatchManager pm;

    // kVariantMap: 0=OPLL, 1=OPLLX, 2=OPLLP, 3=VRC7 (resolveOpllRomVoice()と同じ対応)
    static const uint8_t kVpt[4] = {
        VOICE_PATCH_OPLL, VOICE_PATCH_OPLLX, VOICE_PATCH_OPLLP, VOICE_PATCH_VRC7
    };

    for (uint8_t variantSel = 0; variantSel < 4; ++variantSel) {
        const auto* table = pm.getOpllRomPatches(kVpt[variantSel]);
        REQUIRE(table != nullptr);

        for (uint8_t instIndex = 1; instIndex < 16; ++instIndex) {
            const HwPatch& p = (*table)[instIndex];
            INFO("variantSel=" << (int)variantSel << " instIndex=" << (int)instIndex);
            REQUIRE(p.isValid());
            // bit0=プリセット選択フラグ(常に1)
            CHECK((p.ext.ALG_EXT & 1) == 1);
            // bit2-1=BANK値。OPLLEXがこの値をそのままレジスタ0x40+chへ
            // 書けば、要求元チップと同じROMテーブルを選べる。
            CHECK(((p.ext.ALG_EXT >> 1) & 0x3) == variantSel);

            // hwProgからの直接解決(resolveOpllRomVoice()と同じデコード
            // 規則)でも同じインスタンスが返り、同じBANK値を持つこと。
            uint8_t hwProg = static_cast<uint8_t>((variantSel << 4) | instIndex);
            const HwPatch* byProg = pm.getOpllRomPatchByProg(hwProg);
            REQUIRE(byProg == &p);
        }
    }
}
