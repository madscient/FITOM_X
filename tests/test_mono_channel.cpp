// tests/test_mono_channel.cpp
// モノフォニック(CC#126=1)のCInstChで、前のノートのデバイスチャンネルを
// 引き継いで次のノートを鳴らす経路の回帰テスト。
//
// CInstChはCFITOMのデバイス一覧を引くため、tests/stub_cfitom.hの共通
// フィクスチャ(検証用HWプラグインのOPMとOPL2)を使う。

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "stub_cfitom.h"
#include "fitom/MidiCh.h"
#include "fitom/FITOMdefine.h"

using namespace fitom;
using stub_cfitom::kOpmDevice;
using stub_cfitom::kOpl2Device;
using stub_cfitom::kBank;

namespace {

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
    CFITOM& fitom = stub_cfitom::instance();
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
    CFITOM& fitom = stub_cfitom::instance();
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
    CFITOM& fitom = stub_cfitom::instance();
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
