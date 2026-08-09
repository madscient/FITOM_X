// tests/test_opz_keyon.cpp
// COPZ (YM2414) のキーオン/出力イネーブルに関する回帰テスト。
// OPZ は $08 がチャンネル選択、$20+ch bit6 がキーオン、出力イネーブルが
// $20 bit7 と $30 bit0 の OR という、OPM とは異なるレジスタ割り当てを持つ。
// COPM の実装をそのまま継承すると一度もキーオンされず無音になるため、
// 書き込み内容と順序を固定する。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include <map>
#include <memory>
#include <utility>
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
    // addr への最後の書き込みが history 上の何番目か (未書き込みなら -1)
    int lastIndexOf(uint16_t addr) const {
        for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i)
            if (history[i].first == addr) return i;
        return -1;
    }
};

HwPatch makePatch() {
    HwPatch p{};
    p.id = 1;
    p.hw.ALG = 4;
    p.hw.FB  = 5;
    for (int i = 0; i < 4; ++i) {
        p.hwOp[i].AR = 31; p.hwOp[i].DR = 8; p.hwOp[i].SR = 4;
        p.hwOp[i].SL = 2;  p.hwOp[i].RR = 7; p.hwOp[i].MUL = 1;
        p.hwOp[i].TL = (i % 2) ? 0 : 30;
    }
    return p;
}
} // namespace

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPZ(IPort* p, int sr);
}

TEST_CASE("COPZ key-on uses $20 bit6, not $08 slot mask", "[opz]")
{
    RecordingPort port;
    auto dev = createCOPZ(&port, 44100);
    dev->init();

    HwPatch patch = makePatch();
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    dev->setNoteFine(ch, 60, 0, true);
    dev->noteOn(ch, 100);

    const uint16_t r20 = static_cast<uint16_t>(0x20 + ch);
    REQUIRE((port.regs[r20] & 0x40) != 0);          // キーオン

    // $08 は $20 より前に書かれていること (ymfm は $08 で選択中の
    // チャンネルへの $20 書き込みのみキーオンとして扱う)
    REQUIRE(port.lastIndexOf(0x08) < port.lastIndexOf(r20));

    dev->noteOff(ch);
    REQUIRE((port.regs[r20] & 0x40) == 0);          // キーオフ
    REQUIRE(port.lastIndexOf(0x08) < port.lastIndexOf(r20));
}

TEST_CASE("COPZ re-triggers key-on on the same channel", "[opz]")
{
    RecordingPort port;
    auto dev = createCOPZ(&port, 44100);
    dev->init();

    HwPatch patch = makePatch();
    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    REQUIRE(ch != 0xFF);
    const uint16_t r20 = static_cast<uint16_t>(0x20 + ch);

    dev->setNoteFine(ch, 60, 0, true);
    dev->noteOn(ch, 100);
    dev->noteOff(ch);

    // 同一チャンネルへ同じ値を書き直すケース。シャドウレジスタによる
    // 重複抑止でキーオンのエッジが消えてはいけない。
    const size_t before = port.history.size();
    dev->noteOn(ch, 100);
    bool keyOnWritten = false;
    for (size_t i = before; i < port.history.size(); ++i)
        if (port.history[i].first == r20 && (port.history[i].second & 0x40)) keyOnWritten = true;
    REQUIRE(keyOnWritten);
}

TEST_CASE("COPZ keeps a channel audible at every panpot position", "[opz]")
{
    // 出力イネーブルは $20 bit7 | $30 bit0。どちらも 0 になると無音。
    for (int8_t pan : {static_cast<int8_t>(-64), static_cast<int8_t>(0),
                       static_cast<int8_t>(63)}) {
        RecordingPort port;
        auto dev = createCOPZ(&port, 44100);
        dev->init();

        HwPatch patch = makePatch();
        uint8_t ch = dev->allocCh(nullptr, &patch, 100);
        REQUIRE(ch != 0xFF);
        dev->setPanpot(ch, pan, true);
        dev->setNoteFine(ch, 60, 0, true);
        dev->noteOn(ch, 100);

        const uint8_t v20 = port.regs[static_cast<uint16_t>(0x20 + ch)];
        const uint8_t v30 = port.regs[static_cast<uint16_t>(0x30 + ch)];
        INFO("pan=" << static_cast<int>(pan));
        REQUIRE(((v20 & 0x80) || (v30 & 0x01)));
    }
}
