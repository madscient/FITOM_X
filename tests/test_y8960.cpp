// tests/test_y8960.cpp
// Y8960(MSX用サウンドカートリッジ、統合音源チップ)の拡張ブロック向け
// チップドライバの回帰テスト。
//   - COPLLEX (DEVICE_OPLLEX): 標準OPLL(COPLL)に、チャンネル別ROM
//     プリセットバンク選択レジスタ(0x40+ch)と定位レジスタ(0x50+ch)を
//     追加した拡張。
//   - COPL2EX (DEVICE_OPL2EX): FMコアはOPL2(YM3812)のレジスタマップに
//     定位レジスタ(0xF6+ch、ADPCMは0xFF)を足したもの。
//   - 内蔵リズム(DEVICE_OPLLEX_RHY / DEVICE_OPL2EX_RHY): 使う物理chの
//     定位レジスタを、共有ch(ch7=HH/SD、ch8=CYM/TOM)ではSD/TOMだけが書く。
//   - CSSGEX (DEVICE_SSGEX) / SCCEX (DEVICE_SCCEX): レジスタ配置と定位。
//   - DeviceFactory / FITOMConfig の配線(composite展開、
//     VoicePatchTypeマッピング、フォールバック受け入れ)を確認する。
//
// レジスタ配置の出典は Y8960_Cartridge の doc/spec/Y8960_Specifications.xlsx。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include "fitom/FITOMdefine.h"
#include "fitom/DeviceFactory.h"
#include "fitom/Config.h"
#include "fitom/PatchManager.h"
#include "fitom/VolumeUtils.h"
#include <initializer_list>
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

    // 書かれていない番地はシャドウの初期値と同じ0として読む
    uint8_t at(uint16_t addr) const {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
};

using Addrs = std::initializer_list<uint16_t>;

HwPatch makeOpllUserPatch()
{
    HwPatch p{};
    p.id = 1;
    p.hw.FB = 3;
    p.hwOp[0].TL = 20;
    p.hwOp[1].TL = 40;
    return p;
}

// 内蔵リズムのパート指定 (0=HH,1=CYM,2=TOM,3=SD,4=BD)。パートは hw.ALG で選ぶ。
HwPatch makeRhythmPart(uint8_t part)
{
    HwPatch p{};
    p.id = 1;
    p.hw.ALG = part;
    return p;
}

std::unique_ptr<ISoundDevice> makeDevice(uint32_t deviceType, RecordingPort& port)
{
    auto dev = DeviceFactory::create(deviceType, &port, 44100);
    REQUIRE(dev != nullptr);
    dev->init();
    return dev;
}

// 5パートすべてを割り当てる (ch番号=パート番号になる)
void assignAllRhythmParts(ISoundDevice& dev)
{
    for (uint8_t part = 0; part < 5; ++part) {
        HwPatch p = makeRhythmPart(part);
        REQUIRE(dev.allocCh(nullptr, &p, 100) == part);
    }
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

    // 0xC0+ch: 0x30 | FB<<1 | ALG (bit5-4 はこのチップでは意味を持たない)
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
    // 内蔵リズムは定位レジスタを書くため、標準OPLLとは別のdeviceTypeになる。
    CHECK(spec[1].deviceType == DEVICE_OPLLEX_RHY);

    // 標準OPLLの内蔵リズムはDEVICE_OPLL_RHYのまま。
    spec.clear();
    REQUIRE(FITOMConfig::resolveCompositeSpec(DEVICE_OPLL, true, spec));
    REQUIRE(spec.size() == 2);
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
    // 内蔵リズムは定位レジスタを書くため、標準OPLとは別のdeviceTypeになる。
    CHECK(spec[2].deviceType == DEVICE_OPL2EX_RHY);
}

// ================================================================
//  OPLL系ROM音色 (バンク0固定の暗黙HwPatch) → OPLLEXフォールバック
//
//  PatchManager::initOpllRomPatches()が生成する
//  opllRomPatches_[variantSel][instIndex] は、要求元チップ
//  (OPLL/OPLLX/OPLLP/VRC7)が未接続の場合にOPLLEXへフォールバックできる
//  よう、生成時点でext.ALG_EXTのbit2-1にvariantSelをBANK値として
//  焼き込んでおく設計である(resolveOpllRomVoice()参照)。
//  ここではFITOMConfig(実デバイス)を介さず、PatchManagerの読み取り
//  専用アクセサ(getOpllRomPatches/getOpllRomPatchByProg)経由でこの
//  データが正しく焼き込まれていることを確認する — resolveOpllRomVoice()
//  自体のデバイス選択分岐(接続デバイス一覧が必要)はFITOMConfigに実機/
//  プラグイン無しでデバイスを注入するテスト用の仕組みが無いため、
//  このテストスイートでは直接検証できない(既知のカバレッジ上限)。
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

// ================================================================
//  CC#0=OPLLEXでの暗黙バンク0(ROMプリセット)直接解決
//
//  他のOPLLファミリー(OPLL/OPLLX/OPLLP/VRC7)と同様、OPLLEXも
//  「hwBank==0固定・progだけで暗黙バンク内の音色を一意に解決する」
//  経路(resolveOpllRomVoice())に参加する。
//  resolveOpllRomVoice()自体はprivateで、かつ実際に接続された
//  デバイス一覧(FITOMConfig::devices_)を必要とするが、このテスト
//  スイートにはFITOMConfigへ実機/プラグイン無しでデバイスを注入する
//  仕組みが無いため(test_y8960.cppの他の箇所と同じ既知の制約)、
//  ここではデバイス0台のFITOMConfigを使い、以下2点だけを回帰確認する:
//    1. 経路自体がクラッシュせずreachableであること
//    2. instIndex==0(無音として予約)・BANK>=4(未定義)を正しく
//       「解決失敗」として弾くこと(接続デバイスの有無とは独立した
//       入力検証)
//  実際にOPLLEXデバイスが接続された状態で正しい音色が選ばれることは
//  未検証(実機・エミュレータでの確認が別途必要)。
// ================================================================
TEST_CASE("resolveDirect for OPLLEX's implicit ROM bank rejects invalid prog encodings",
          "[y8960][opllex][patchmanager]")
{
    FITOMConfig cfg;   // デバイス0台
    PatchManager pm;
    Patch storage;

    // instIndex==0(下位4bit)は無音として予約 (BANK=0)
    {
        auto r = pm.resolveDirect(VOICE_PATCH_OPLLEX, 0, 0x00, cfg, storage);
        CHECK_FALSE(r.isValid());
    }
    // BANK(上位3bit)>=4は未定義
    for (uint8_t bank : {uint8_t{4}, uint8_t{5}, uint8_t{6}, uint8_t{7}}) {
        uint8_t prog = static_cast<uint8_t>((bank << 4) | 1);
        INFO("bank=" << (int)bank);
        auto r = pm.resolveDirect(VOICE_PATCH_OPLLEX, 0, prog, cfg, storage);
        CHECK_FALSE(r.isValid());
    }
    // 有効なprog(BANK=0-3、inst=1-15)は、接続デバイスが無いため
    // 「解決失敗」になる(クラッシュしないことを確認する)。
    for (uint8_t bank = 0; bank < 4; ++bank) {
        uint8_t prog = static_cast<uint8_t>((bank << 4) | 3);
        INFO("bank=" << (int)bank);
        auto r = pm.resolveDirect(VOICE_PATCH_OPLLEX, 0, prog, cfg, storage);
        CHECK_FALSE(r.isValid());
    }
}

// ================================================================
//  Y8960 の 8bit 定位 (1=左端 / 128=中央 / 255=右端)
// ================================================================

TEST_CASE("y8960PanValue maps the MIDI panpot onto 1 / 128 / 255", "[y8960][pan]")
{
    CHECK(fitom::y8960PanValue(-64) == 1);
    CHECK(fitom::y8960PanValue(0)   == 128);
    CHECK(fitom::y8960PanValue(63)  == 255);

    int prev = 0;
    for (int p = -64; p <= 63; ++p) {
        const int v = fitom::y8960PanValue(static_cast<int8_t>(p));
        INFO("panpot=" << p << " value=" << v);
        CHECK(v > prev);   // 0 (仕様に無い値) を出さず、単調に増える
        prev = v;
    }
}

TEST_CASE("COPLLEX writes the stereo position to 0x50+ch", "[y8960][opllex][pan]")
{
    RecordingPort port;
    auto dev = createCOPLLEX(&port, 44100, 0);
    dev->init();
    for (uint8_t ch = 0; ch < 9; ++ch) {
        INFO("ch=" << (int)ch);
        CHECK(port.regs.at(static_cast<uint16_t>(0x50 + ch)) == 128);
    }

    HwPatch patch = makeOpllUserPatch();
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setPanpot(ch, -64);
    CHECK(port.regs.at(static_cast<uint16_t>(0x50 + ch)) == 1);
    dev->setPanpot(ch, 63);
    CHECK(port.regs.at(static_cast<uint16_t>(0x50 + ch)) == 255);
}

// 標準OPLLは定位レジスタを持たないため、0x40以降に一切書かない。
TEST_CASE("Plain OPLL does not touch the Y8960 BANK/position registers", "[y8960][opllex][pan]")
{
    RecordingPort port;
    auto dev = makeDevice(DEVICE_OPLL, port);
    HwPatch patch = makeOpllUserPatch();
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setPanpot(ch, -64);
    for (const auto& [addr, data] : port.regs) {
        INFO("addr=0x" << std::hex << addr);
        CHECK(addr < 0x40);
    }
}

// 共有ch(ch7=HH/SD、ch8=CYM/TOM)の定位はSD/TOMだけが書く。
TEST_CASE("OPLLEX rhythm writes positions for BD/SD/TOM only", "[y8960][opllex][rhythm][pan]")
{
    RecordingPort port;
    auto dev = makeDevice(DEVICE_OPLLEX_RHY, port);
    CHECK(dev->getDeviceType() == DEVICE_OPLLEX_RHY);
    CHECK(dev->getChCount() == 5);
    assignAllRhythmParts(*dev);
    CHECK(port.regs.at(0x56) == 128);   // BD = ch6
    CHECK(port.regs.at(0x57) == 128);   // SD = ch7
    CHECK(port.regs.at(0x58) == 128);   // TOM = ch8

    port.regs.clear();
    dev->setPanpot(0, -64);             // HH
    dev->setPanpot(1, -64);             // CYM
    CHECK(port.regs.count(0x57) == 0);
    CHECK(port.regs.count(0x58) == 0);

    dev->setPanpot(3, 63);              // SD
    CHECK(port.regs.at(0x57) == 255);
    dev->setPanpot(2, -64);             // TOM
    CHECK(port.regs.at(0x58) == 1);
    dev->setPanpot(4, 63);              // BD
    CHECK(port.regs.at(0x56) == 255);
}

TEST_CASE("Plain OPLL rhythm does not write position registers", "[y8960][opllex][rhythm][pan]")
{
    RecordingPort port;
    auto dev = makeDevice(DEVICE_OPLL_RHY, port);
    assignAllRhythmParts(*dev);
    for (uint8_t part = 0; part < 5; ++part) dev->setPanpot(part, 63);
    for (uint16_t addr : Addrs{0x56, 0x57, 0x58}) {
        INFO("addr=0x" << std::hex << addr);
        CHECK(port.regs.count(addr) == 0);
    }
}

// ================================================================
//  COPL2EX / 内蔵リズム / 内蔵ADPCM-B の定位
// ================================================================

TEST_CASE("COPL2EX writes the stereo position to 0xF6+ch", "[y8960][opl2ex][pan]")
{
    RecordingPort port;
    auto dev = createCOPL2EX(&port, 44100, false);
    dev->init();
    for (uint8_t ch = 0; ch < 9; ++ch) {
        INFO("ch=" << (int)ch);
        CHECK(port.regs.at(static_cast<uint16_t>(0xF6 + ch)) == 128);
    }

    HwPatch patch{};
    patch.id = 1;
    patch.hw.FB = 5;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    const uint8_t c0 = port.regs.at(static_cast<uint16_t>(0xC0 + ch));

    dev->setPanpot(ch, -64);
    CHECK(port.regs.at(static_cast<uint16_t>(0xF6 + ch)) == 1);
    dev->setPanpot(ch, 63);
    CHECK(port.regs.at(static_cast<uint16_t>(0xF6 + ch)) == 255);
    // OPL3のL/R出力ビットの位置(0xC0+chのbit5-4)には定位を書かない
    CHECK(port.regs.at(static_cast<uint16_t>(0xC0 + ch)) == c0);
}

TEST_CASE("Plain OPL2 does not write the Y8960 position registers", "[y8960][opl2ex][pan]")
{
    RecordingPort port;
    auto dev = makeDevice(DEVICE_OPL2, port);
    HwPatch patch{};
    patch.id = 1;
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setPanpot(ch, -64);
    for (uint16_t addr = 0xF6; addr <= 0xFF; ++addr) {
        INFO("addr=0x" << std::hex << addr);
        CHECK(port.regs.count(addr) == 0);
    }
}

TEST_CASE("OPL2EX rhythm writes positions for BD/SD/TOM only", "[y8960][opl2ex][rhythm][pan]")
{
    RecordingPort port;
    auto dev = makeDevice(DEVICE_OPL2EX_RHY, port);
    CHECK(dev->getDeviceType() == DEVICE_OPL2EX_RHY);
    CHECK(dev->getChCount() == 5);
    assignAllRhythmParts(*dev);
    CHECK(port.regs.at(0xFC) == 128);   // BD = ch6
    CHECK(port.regs.at(0xFD) == 128);   // SD = ch7
    CHECK(port.regs.at(0xFE) == 128);   // TOM = ch8

    port.regs.clear();
    dev->setPanpot(0, -64);             // HH
    dev->setPanpot(1, -64);             // CYM
    CHECK(port.regs.count(0xFD) == 0);
    CHECK(port.regs.count(0xFE) == 0);

    dev->setPanpot(3, 63);              // SD
    CHECK(port.regs.at(0xFD) == 255);
    dev->setPanpot(2, -64);             // TOM
    CHECK(port.regs.at(0xFE) == 1);
    dev->setPanpot(4, 63);              // BD
    CHECK(port.regs.at(0xFC) == 255);
}

TEST_CASE("OPL2EX ADPCM-B writes its stereo position to 0xFF", "[y8960][opl2ex][adpcm][pan]")
{
    RecordingPort port;
    auto dev = makeDevice(DEVICE_ADPCMB_OPL2EX, port);
    CHECK(port.regs.at(0xFF) == 128);
    dev->setPanpot(0, -64);
    CHECK(port.regs.at(0xFF) == 1);
    dev->setPanpot(0, 63);
    CHECK(port.regs.at(0xFF) == 255);

    // Y8950のADPCM-Bは定位レジスタを持たない
    RecordingPort y8950Port;
    auto y8950 = makeDevice(DEVICE_ADPCMB_Y8950, y8950Port);
    y8950->setPanpot(0, -64);
    CHECK(y8950Port.regs.count(0xFF) == 0);
}

// ================================================================
//  CSSGEX (Y8960 SSG部)
// ================================================================

TEST_CASE("CSSGEX lays out SSG-2 at 0x10 with per-channel volume and position",
          "[y8960][ssgex]")
{
    RecordingPort port;
    port.clock = 1789772;   // SSG ブロックの動作クロックそのもの
    auto dev = makeDevice(DEVICE_SSGEX, port);
    CHECK(dev->getDeviceType() == DEVICE_SSGEX);
    CHECK(dev->getChCount() == 6);

    // init: 両ユニットのミキサーを全ch無効、chごとの音量は等倍、定位は中央
    CHECK(port.regs.at(0x07) == 0x3F);
    CHECK(port.regs.at(0x17) == 0x3F);
    for (uint16_t addr : Addrs{0x20, 0x21, 0x22, 0x24, 0x25, 0x26,
                               0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E}) {
        INFO("addr=0x" << std::hex << addr);
        CHECK(port.regs.at(addr) == 128);
    }
    // 未接続の番地と、YMZ705 の配置 (SSG-2 が 0x20〜、定位が 0x30〜) には書かない
    for (uint16_t addr : Addrs{0x23, 0x27, 0x2B, 0x2F, 0x30, 0x31, 0x32}) {
        INFO("addr=0x" << std::hex << addr);
        CHECK(port.regs.count(addr) == 0);
    }

    HwPatch p{};
    p.id = 1;
    p.hwOp[0].AR = 31;
    for (uint8_t ch = 0; ch < 6; ++ch) {
        REQUIRE(dev->assignCh(ch, nullptr, &p, 100) == ch);
        dev->setNoteFine(ch, 69, 0, true);
    }
    // A4 のトーン周期: 1789772 / (16 * 440) = 254.2。クロックを換算しないこと
    CHECK(port.at(0x00) == 254);
    CHECK((port.at(0x01) & 0x0F) == 0);
    // ch4 = SSG-2 の CH B → 0x12/0x13
    CHECK(port.at(0x12) == 254);

    dev->setPanpot(0, -64);
    CHECK(port.regs.at(0x28) == 1);
    dev->setPanpot(4, 63);
    CHECK(port.regs.at(0x2D) == 255);
}

// ================================================================
//  SCCEX (Y8960 SCC部)
// ================================================================

TEST_CASE("SCCEX keeps the K051649 layout and adds positions at 0x100+ch",
          "[y8960][sccex]")
{
    RecordingPort port;
    port.clock = 3579545;
    auto dev = makeDevice(DEVICE_SCCEX, port);
    CHECK(dev->getDeviceType() == DEVICE_SCCEX);
    CHECK(dev->getChCount() == 5);

    // 無印SCC (K051649) と同じ制御レジスタ配置
    CHECK(port.regs.count(0x8A) > 0);   // ch0 音量
    CHECK(port.regs.count(0x8F) > 0);   // ch 有効
    CHECK(port.regs.count(0xAF) == 0);  // SCC+ の配置には書かない
    for (uint8_t ch = 0; ch < 5; ++ch) {
        INFO("ch=" << (int)ch);
        CHECK(port.regs.at(static_cast<uint16_t>(0x100 + ch)) == 128);
    }

    dev->setPanpot(2, -64);
    CHECK(port.regs.at(0x102) == 1);
    dev->setPanpot(4, 63);
    CHECK(port.regs.at(0x104) == 255);

    // ch3/ch4 の波形メモリ共有も無印SCCと同じ: ch3 と違う波形の ch4 は割り当てない
    HwPatch w1{};
    w1.id = 1;
    w1.hwOp[0].WS = 1;
    HwPatch w2 = w1;
    w2.hwOp[0].WS = 2;
    for (uint8_t i = 0; i < 4; ++i) {
        REQUIRE(dev->allocCh(nullptr, &w1, 100) == i);
        dev->noteOn(i, 100);
    }
    CHECK(dev->queryCh(nullptr, &w2, 1) == 0xFF);   // mode=1: 奪わずに探す
    CHECK(dev->queryCh(nullptr, &w1, 1) == 4);

    // 無印SCCは定位を書かない
    RecordingPort sccPort;
    sccPort.clock = 3579545;
    auto scc = makeDevice(DEVICE_SCC, sccPort);
    scc->setPanpot(0, -64);
    CHECK(sccPort.regs.count(0x100) == 0);
}

// ================================================================
//  設定側の索引
// ================================================================

TEST_CASE("FITOMConfig indexes the Y8960 blocks as continuous-pan devices",
          "[y8960][config][pan]")
{
    for (uint32_t t : {DEVICE_OPLLEX, DEVICE_OPLLEX_RHY, DEVICE_OPL2EX, DEVICE_OPL2EX_RHY,
                       DEVICE_ADPCMB_OPL2EX, DEVICE_SSGEX, DEVICE_SCCEX}) {
        INFO("deviceType=" << t);
        CHECK(FITOMConfig::getChipPanType(t) == FITOMConfig::ChipPanType::Continuous);
    }
    // ハードウェアの定位を持つため、CLinearPanDevice の束ね対象にしない
    for (uint32_t t : {DEVICE_OPLLEX, DEVICE_OPLLEX_RHY, DEVICE_OPL2EX, DEVICE_OPL2EX_RHY,
                       DEVICE_ADPCMB_OPL2EX}) {
        INFO("deviceType=" << t);
        CHECK_FALSE(FITOMConfig::subDeviceAcceptsStereoPair(t));
    }
    // 標準のリズムはモノラル扱い・束ね対象のまま
    CHECK(FITOMConfig::getChipPanType(DEVICE_OPLL_RHY) == FITOMConfig::ChipPanType::Mono);
    CHECK(FITOMConfig::subDeviceAcceptsStereoPair(DEVICE_OPLL_RHY));
}

TEST_CASE("FITOMConfig maps the new Y8960 device types to shared VoicePatchTypes",
          "[y8960][config]")
{
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SSGEX) == VOICE_PATCH_SSG);
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_SCCEX) == VOICE_PATCH_SCC);
    CHECK(FITOMConfig::deviceTypeToVoicePatchType(DEVICE_OPL2EX_RHY) == VOICE_PATCH_OPL_RHY);
    CHECK(DeviceFactory::defaultChCount(DEVICE_SSGEX) == 6);
    CHECK(DeviceFactory::defaultChCount(DEVICE_SCCEX) == 5);
    CHECK(DeviceFactory::defaultChCount(DEVICE_OPLLEX_RHY) == 5);
    CHECK(DeviceFactory::defaultChCount(DEVICE_OPL2EX_RHY) == 5);
}
