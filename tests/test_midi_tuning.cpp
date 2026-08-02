// tests/test_midi_tuning.cpp
// チューニング系(RPNデータエントリー ⇔ kfs単位のピッチオフセット)の回帰テスト。
//
// ユーザー報告(2026年8月): RPN#1(チャンネルファインチューニング)で
// 指定した値と、実際に鳴るピッチが食い違う。
//
// 調査の結果、MidiProcessor::processControl()がData Entry MSB(CC#6)受信時
// にしかRPNを適用しておらず、通常の送信順(CC#6 → CC#38)で送られた
// Data Entry LSB(CC#38)が一切反映されていなかった(しかもCC#6適用時に
// 参照されるLSBは「前回別のパラメータへ書いた際の残骸」だった)。
// RPN#1は14bit全体を使うパラメータのため、この取りこぼしがそのまま
// ピッチのずれとして出る。

#include <catch2/catch_test_macros.hpp>
#include "fitom/CFITOM.h"
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/MidiCh.h"
#include "fitom/VoiceData.h"
#include <array>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace fitom;

namespace {

// setRPNRegister()/setNRPNRegister()へ最終的に渡された値だけを記録する
// スパイ。MidiProcessorのデータエントリー合成ロジック単体を検証する。
struct SpyMidiCh : IMidiCh {
    std::vector<std::pair<uint16_t, uint16_t>> rpnCalls;   // (reg, 14bit値)
    std::vector<std::pair<uint16_t, uint16_t>> nrpnCalls;

    void timerCallback(uint32_t) override {}
    void setRPNRegister(uint16_t reg, uint16_t val) override  { rpnCalls.emplace_back(reg, val); }
    void setNRPNRegister(uint16_t reg, uint16_t val) override { nrpnCalls.emplace_back(reg, val); }
};

// MidiProcessorをCFITOM本体から切り離して組み立てるためのフィクスチャ。
// CC#6/38/96-101の経路はparent_を一切参照しないため、parent=nullptrで足りる。
struct RpnFixture {
    std::array<std::unique_ptr<IMidiCh>, 16> channels;
    MidiProcessor mp;
    SpyMidiCh* spy;

    RpnFixture()
        : channels{}, mp(channels, nullptr, /*clockEnabled=*/false)
    {
        auto ch0 = std::make_unique<SpyMidiCh>();
        spy = ch0.get();
        channels[0] = std::move(ch0);
    }

    void cc(uint8_t num, uint8_t val) { mp.sendControlChange(0, num, val); }

    // RPN番号を選択する (CC#101=MSB, CC#100=LSB)
    void selectRpn(uint8_t msb, uint8_t lsb) { cc(101, msb); cc(100, lsb); }
};

class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
    void write(uint16_t addr, uint16_t data) override {
        regs[addr] = static_cast<uint8_t>(data);
    }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
};

} // namespace

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPN(IPort* p, int sr);
}

// ----------------------------------------------------------------
//  MidiProcessor: データエントリー(CC#6/CC#38)の合成
// ----------------------------------------------------------------

// 本命の回帰テスト。CC#6 → CC#38 という通常の送信順で、最終的に
// 14bit値全体がRPNへ届いていること。
TEST_CASE("RPN Data Entry LSB (CC#38) sent after MSB (CC#6) reaches the parameter",
          "[midi][rpn][tuning]")
{
    RpnFixture f;
    f.selectRpn(0, 1);          // RPN#1: Channel Fine Tuning
    f.cc(6, 100);               // Data Entry MSB
    f.cc(38, 42);               // Data Entry LSB

    REQUIRE(f.spy->rpnCalls.size() == 2); // MSB受信時とLSB受信時の2回
    // 1回目: MSBのみ(LSBはパラメータ選択時にクリアされているので0)
    CHECK(f.spy->rpnCalls[0].first  == 0x0001);
    CHECK(f.spy->rpnCalls[0].second == static_cast<uint16_t>(100 << 7));
    // 2回目: MSB/LSB合成後の14bit値。修正前はこの呼び出し自体が無く、
    // LSBは次のCC#6まで一切反映されなかった。
    CHECK(f.spy->rpnCalls[1].first  == 0x0001);
    CHECK(f.spy->rpnCalls[1].second == static_cast<uint16_t>((100 << 7) | 42));
}

// パラメータ番号を選び直したら、前のパラメータへ書いたデータエントリーの
// 残骸(特にLSB)を引き継いではならない。修正前は rpn_[ch].lsb が
// クリアされず、直前のRPNのLSBが次のRPNのMSB適用時に紛れ込んでいた。
TEST_CASE("Selecting a new RPN clears the retained Data Entry LSB", "[midi][rpn][tuning]")
{
    RpnFixture f;
    f.selectRpn(0, 1);
    f.cc(6, 64);
    f.cc(38, 127);              // RPN#1へLSB=127を書く

    f.selectRpn(0, 0);          // RPN#0: Pitch Bend Sensitivity を選び直す
    f.cc(6, 2);                 // MSBのみ送る(LSBは送らない)

    REQUIRE_FALSE(f.spy->rpnCalls.empty());
    const auto& last = f.spy->rpnCalls.back();
    CHECK(last.first  == 0x0000);
    CHECK(last.second == static_cast<uint16_t>(2 << 7)); // LSBは0。127が残っていてはならない
}

// パラメータ選択直後にCC#38だけが来た場合(MSB未受信)は適用しない。
// MSBを0とみなして適用すると、中央値0x2000が前提のRPN#1等で
// ピッチが1半音近く飛んでしまう。
TEST_CASE("Data Entry LSB alone (no MSB since parameter select) does not apply",
          "[midi][rpn][tuning]")
{
    RpnFixture f;
    f.selectRpn(0, 1);
    f.cc(38, 64);
    CHECK(f.spy->rpnCalls.empty());
}

// RPN NULL (127,127) 選択中はデータエントリーを一切適用しない(MIDI規格)。
TEST_CASE("Data Entry is ignored while RPN NULL (127,127) is selected", "[midi][rpn]")
{
    RpnFixture f;
    f.selectRpn(127, 127);
    f.cc(6, 100);
    f.cc(38, 42);
    CHECK(f.spy->rpnCalls.empty());
    CHECK(f.spy->nrpnCalls.empty());
}

// NRPN側も同じ経路を通る(CC#6でMSBのみ、CC#38で合成値)。
// リズムチャンネルのGM2ドラムNRPN等はMSBのみ使う実装だが、
// 合成値がそのまま渡ること自体はRPNと共通の契約。
TEST_CASE("NRPN Data Entry goes through the same MSB/LSB assembly", "[midi][nrpn]")
{
    RpnFixture f;
    f.cc(99, 24); f.cc(98, 36);  // NRPN 24,36 (ドラムインストゥルメントピッチ)
    f.cc(6, 70);
    f.cc(38, 5);

    REQUIRE(f.spy->nrpnCalls.size() == 2);
    CHECK(f.spy->nrpnCalls[0].first  == static_cast<uint16_t>((24 << 7) | 36));
    CHECK(f.spy->nrpnCalls[0].second == static_cast<uint16_t>(70 << 7));
    CHECK(f.spy->nrpnCalls[1].second == static_cast<uint16_t>((70 << 7) | 5));
    CHECK(f.spy->rpnCalls.empty());
}

// ----------------------------------------------------------------
//  kfs単位の契約: 1半音 = 64ステップ
// ----------------------------------------------------------------

// CInstCh側のチューニング換算(RPN#1の±8192 → ±64ステップ = ±100cents、
// RPN#2の1半音 → 64ステップ)は、全て「setNoteFine()のfine引数は
// 1半音=64ステップ」という前提に依存している。この前提が崩れると
// 指定値と実際のピッチがそのままずれるため、契約として固定しておく。
TEST_CASE("setNoteFine: +64 kfs equals exactly one semitone up", "[sounddevice][tuning]")
{
    RecordingPort port;
    auto dev = createCOPN(&port, 8000000);
    dev->init();

    HwPatch patch{};
    patch.id = 1;

    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);

    auto fnumFor = [&](uint8_t note, int16_t fine) {
        dev->setNoteFine(ch, note, fine, true);
        const auto* cs = dev->getChState(ch);
        REQUIRE(cs != nullptr);
        return std::pair<uint8_t, uint16_t>{cs->lastFnum.block, cs->lastFnum.fnum};
    };

    // note=60 を +64kfs した音程は note=61 を ±0 した音程と一致する
    CHECK(fnumFor(60, 64) == fnumFor(61, 0));
    // 逆方向も同じ
    CHECK(fnumFor(60, -64) == fnumFor(59, 0));
    // 1オクターブ(12半音 = 768ステップ)
    CHECK(fnumFor(60, 768) == fnumFor(72, 0));

    // ±64kfs未満は隣の半音には達しない(=RPN#1のフルスケールが
    // ちょうど±1半音に収まっていることの裏付け)
    CHECK(fnumFor(60, 63) != fnumFor(61, 0));
}
