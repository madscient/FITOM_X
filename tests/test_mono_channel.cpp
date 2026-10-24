// tests/test_mono_channel.cpp
// モノフォニック(CC#126=1)のCInstChで、前のノートのデバイスチャンネルを
// 引き継いで次のノートを鳴らす経路の回帰テスト。
//
// CInstChはCFITOMのデバイス一覧を引くため、検証用HWプラグイン
// (tests/stub_hwplugin)でOPMとOPL2を1台ずつ持つプロファイルを組み、
// CFITOM(シングルトン)をこのファイルで1回だけ初期化する。
// 検証用プラグインはレジスタ書き込みを記録しないので、判定はデバイスの
// チャンネル状態(ChState)で行う。

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "fitom/CFITOM.h"
#include "fitom/Config.h"
#include "fitom/MidiCh.h"
#include "fitom/PatchManager.h"
#include "fitom/FITOMdefine.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <memory>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace fitom;

namespace {

constexpr int kOpmDevice  = 0;
constexpr int kOpl2Device = 1;
constexpr uint8_t kBank   = 1;

void writeJson(const fs::path& p, const json& j)
{
    std::ofstream f(p);
    f << j.dump(2);
}

json stubDevice(const char* chip)
{
    return { {"if", "HW"}, {"plugin", "stub"}, {"type", "FMHWIF"}, {"engine", "Stub"},
             {"chip", chip}, {"index", 0}, {"label", chip}, {"clock", 3579545} };
}

CFITOM& fitomForMonoTests()
{
    static bool initialized = false;
    CFITOM& fitom = CFITOM::instance();
    if (initialized) return fitom;

    fs::path dir = fs::temp_directory_path() / "fitom_test_mono_channel";
    fs::create_directories(dir);

    // OPM bank1: prog0=ソフトウェアLFOつき(sw bank0/prog0)、prog1=LFOなし
    writeJson(dir / "opm.hwbank.json", {
        {"name", "OPM mono test"},
        {"patches", json::array({
            {{"prog", 0}, {"name", "OPM A (LFO)"}, {"sw_bank", 0}, {"sw_prog", 0}},
            {{"prog", 1}, {"name", "OPM B"}}
        })}
    });
    writeJson(dir / "opl2.hwbank.json", {
        {"name", "OPL2 mono test"},
        {"patches", json::array({ {{"prog", 0}, {"name", "OPL2 C"}} })}
    });
    writeJson(dir / "lfo.swbank.json", {
        {"name", "LFO"},
        {"patches", json::array({
            {{"prog", 0}, {"name", "vibrato"},
             {"sw", {{"LFR", 64}, {"LFD", 0}, {"LFI", 0}, {"LWF", 6}, {"LFM", 0},
                     {"depth_cents", 50}}}}
        })}
    });

    json profile = {
        {"profile_name", "mono channel test"},
        {"hw_plugins",   json::array({ {{"name", "stub"}, {"dll", FITOM_TEST_HWPLUGIN_PARTS}} })},
        {"devices",      json::array({ stubDevice("OPM"), stubDevice("OPL2") })},
        {"banks", {
            {"hw_banks", json::array({
                {{"group", "OPM"},  {"bank", kBank}, {"file", "opm.hwbank.json"}},
                {{"group", "OPL2"}, {"bank", kBank}, {"file", "opl2.hwbank.json"}}
            })},
            {"sw_banks", json::array({ {{"bank", 0}, {"file", "lfo.swbank.json"}} })}
        }}
    };
    fs::path profilePath = dir / "mono.profile.json";
    writeJson(profilePath, profile);

    auto cfg = std::make_unique<FITOMConfig>();
    auto pm  = std::make_unique<PatchManager>();
    REQUIRE(cfg->loadProfile(profilePath, pm.get()));
    REQUIRE(cfg->getDeviceCount() == 2);
    fitom.init(std::move(cfg), std::move(pm));
    initialized = true;
    return fitom;
}

CInstCh& monoChannel(CFITOM& fitom, uint8_t ch, bool legato)
{
    auto* inst = dynamic_cast<CInstCh*>(fitom.getMidiProcessor(0)->getChannel(ch));
    REQUIRE(inst != nullptr);
    inst->setPolyMode();
    inst->setMonoMode(1);
    inst->setLegato(legato);
    return *inst;
}

void selectDirect(CInstCh& inst, uint8_t voicePatchType, uint8_t prog)
{
    inst.bankSelMSB(voicePatchType);
    inst.bankSelLSB(kBank);
    inst.progChange(prog);
}

// このMIDIチャンネルが鳴らしている(Running)デバイスチャンネルを1つ探す。
const ChState* runningChOf(CFITOM& fitom, int devIndex, const IMidiCh* owner)
{
    ISoundDevice* dev = fitom.getDevice(devIndex);
    REQUIRE(dev != nullptr);
    for (uint8_t c = 0; c < dev->getChCount(); ++c) {
        const ChState* s = dev->getChState(c);
        if (s && s->isRunning() && s->owner == owner) return s;
    }
    return nullptr;
}

uint32_t patchId(CFITOM& fitom, uint8_t voicePatchType, uint8_t prog)
{
    const HwBank* bank = fitom.getPatchManager().hwRegistry().find(voicePatchType, kBank);
    REQUIRE(bank != nullptr);
    REQUIRE(bank->get(prog).isValid());
    return bank->get(prog).id;
}

} // namespace

TEST_CASE("Mono channel: a note after a program change uses the new patch "
          "while the previous note is held", "[midi][mono]")
{
    CFITOM& fitom = fitomForMonoTests();
    const bool legato = GENERATE(false, true);
    INFO("legato=" << legato);
    CInstCh& inst = monoChannel(fitom, 0, legato);

    selectDirect(inst, VOICE_PATCH_OPM, 0);
    inst.noteOn(60, 100);
    selectDirect(inst, VOICE_PATCH_OPM, 1);   // 60を押さえたまま
    inst.noteOn(62, 100);

    const ChState* s = runningChOf(fitom, kOpmDevice, &inst);
    REQUIRE(s != nullptr);
    CHECK(s->lastNote == 62);
    CHECK(s->hwPatch.id == patchId(fitom, VOICE_PATCH_OPM, 1));
    CHECK_FALSE(s->proc.channelLfoActive());   // prog1はLFOを持たない

    inst.allNoteOff();
}

TEST_CASE("Mono channel: re-striking without legato restarts the software LFO",
          "[midi][mono]")
{
    CFITOM& fitom = fitomForMonoTests();
    CInstCh& inst = monoChannel(fitom, 1, /*legato=*/false);

    selectDirect(inst, VOICE_PATCH_OPM, 0);
    inst.noteOn(60, 100);
    inst.noteOn(62, 100);

    // 前のノートのキーオフで始まるLFOのフェードアウト(kReleasingHoldMs)が
    // 終わるまで進めても、新しいノートのLFOは動いていなければならない。
    ISoundDevice* dev = fitom.getDevice(kOpmDevice);
    for (uint32_t t = 0; t < ChState::kReleasingHoldMs + 100; ++t) dev->timerCallback(t);

    const ChState* s = runningChOf(fitom, kOpmDevice, &inst);
    REQUIRE(s != nullptr);
    CHECK(s->lastNote == 62);
    CHECK(s->proc.channelLfoActive());

    inst.allNoteOff();
}

TEST_CASE("Mono channel: when a program change moves the patch to another chip, "
          "the previous note on the old chip is released", "[midi][mono]")
{
    CFITOM& fitom = fitomForMonoTests();
    const bool legato = GENERATE(false, true);
    INFO("legato=" << legato);
    CInstCh& inst = monoChannel(fitom, 2, legato);

    selectDirect(inst, VOICE_PATCH_OPM, 1);
    inst.noteOn(60, 100);
    REQUIRE(runningChOf(fitom, kOpmDevice, &inst) != nullptr);

    selectDirect(inst, VOICE_PATCH_OPL2, 0);  // 60を押さえたまま
    inst.noteOn(62, 100);

    CHECK(runningChOf(fitom, kOpmDevice, &inst) == nullptr);
    const ChState* s = runningChOf(fitom, kOpl2Device, &inst);
    REQUIRE(s != nullptr);
    CHECK(s->lastNote == 62);
    CHECK(s->hwPatch.id == patchId(fitom, VOICE_PATCH_OPL2, 0));

    inst.allNoteOff();
}
