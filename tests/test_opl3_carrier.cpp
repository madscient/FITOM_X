// tests/test_opl3_carrier.cpp
// COPL3(OPL3 4OP)のキャリアオペレータ判定(carmsk[8])に関する回帰テスト。
//
// 2026年8月、ConnectionSEL無効時(hw.ALG bit2=0、前半・後半が独立した
// 2OPペア×2)のcarmskエントリ(インデックス0-3)が、前半ペア単体または
// 後半ペア単体のキャリアマスクしか持っておらず、もう一方のペアの
// キャリアopがボリューム/ベロシティの影響を一切受けないバグがあった。
// ConnectionSEL有効時(インデックス4-7、実際に全バンクが使用する値)は
// Nuked-OPL3の実装と突き合わせて検証済みで、この回帰テストでも現状維持
// (壊れていないこと)を確認する。
//
// 「キャリアである」ことは、setVolume()でTLレジスタが変化するかどうかで
// 判定する(モジュレータはupdateVoice()内で固定TLを一度書くだけで、
// updateVolExp()の対象にならないため、ボリューム変更ではレジスタ値が
// 変化しない)。

#include <catch2/catch_test_macros.hpp>
#include "fitom/ISoundDevice.h"
#include "fitom/IPort.h"
#include "fitom/VoiceData.h"
#include <map>
#include <memory>
#include <utility>

using namespace fitom;

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPL3(IPort* p, int sr);
}

namespace {
class RecordingPort : public IPort {
public:
    std::map<uint16_t, uint8_t> regs;
    void write(uint16_t addr, uint16_t data) override { regs[addr] = static_cast<uint8_t>(data); }
    uint8_t read(uint16_t addr) override {
        auto it = regs.find(addr);
        return it != regs.end() ? it->second : 0;
    }
};

// opmap[4] = {0x0, 0x3, 0x8, 0xb} (COPL3::opmap、dch=0固定で検証)
constexpr uint16_t kTlReg[4] = {0x40, 0x43, 0x48, 0x4b};

// op(0-3)がAL(ALG値)においてキャリアかどうかを、
// vol=127→vol=1でTLレジスタが変化するかで判定する。
bool isCarrierByObservation(uint8_t al, int op) {
    RecordingPort port;
    auto dev = fitom::createCOPL3(&port, 8000000);
    dev->init();

    HwPatch patch{};
    patch.id = 1;
    patch.hw.ALG = al;
    for (int i = 0; i < 4; ++i) {
        patch.hwOp[i].TL = 32;
        patch.hwOp[i].AR = 31;
        patch.hwOp[i].RR = 8;
        patch.hwOp[i].MUL = 1;
    }

    uint8_t ch = dev->allocCh(nullptr, &patch, 100);
    if (ch == 0xFF) return false;
    dev->setNoteFine(ch, 60, 0, true);
    dev->setVolume(ch, 127, true);
    dev->noteOn(ch, 100);
    uint8_t before = port.regs[kTlReg[op]];

    dev->setVolume(ch, 1, true);
    uint8_t after = port.regs[kTlReg[op]];

    return before != after;
}
} // namespace

TEST_CASE("COPL3 carmsk: ConnectionSEL有効時(AL 4-7)は実機と一致", "[sounddevice][opl3]")
{
    // AL=4: 全直列(op0->op1->op2->op3)、キャリアはop3のみ
    CHECK_FALSE(isCarrierByObservation(4, 0));
    CHECK_FALSE(isCarrierByObservation(4, 1));
    CHECK_FALSE(isCarrierByObservation(4, 2));
    CHECK(isCarrierByObservation(4, 3));

    // AL=5: op0(独立) + op1->op2->op3、キャリアはop0,op3
    CHECK(isCarrierByObservation(5, 0));
    CHECK_FALSE(isCarrierByObservation(5, 1));
    CHECK_FALSE(isCarrierByObservation(5, 2));
    CHECK(isCarrierByObservation(5, 3));

    // AL=6: (op0->op1) + (op2->op3)、キャリアはop1,op3
    CHECK_FALSE(isCarrierByObservation(6, 0));
    CHECK(isCarrierByObservation(6, 1));
    CHECK_FALSE(isCarrierByObservation(6, 2));
    CHECK(isCarrierByObservation(6, 3));

    // AL=7: op0(独立) + (op1->op2) + op3(独立)、キャリアはop0,op2,op3
    CHECK(isCarrierByObservation(7, 0));
    CHECK_FALSE(isCarrierByObservation(7, 1));
    CHECK(isCarrierByObservation(7, 2));
    CHECK(isCarrierByObservation(7, 3));
}

TEST_CASE("COPL3 carmsk: ConnectionSEL無効時(AL 0-3、独立2OPペア×2)は両ペアのキャリアが効く",
          "[sounddevice][opl3]")
{
    // AL=0: 前半CON=0(直列、キャリア=op1) + 後半CON=0(直列、キャリア=op3)
    // 旧実装は前半ペアのみ(0x2)を見ており、op3がキャリア扱いされず
    // ボリュームが効かないバグがあった。
    CHECK_FALSE(isCarrierByObservation(0, 0));
    CHECK(isCarrierByObservation(0, 1));
    CHECK_FALSE(isCarrierByObservation(0, 2));
    CHECK(isCarrierByObservation(0, 3));

    // AL=1: 前半CON=1(並列、キャリア=op0,op1) + 後半CON=0(直列、キャリア=op3)
    CHECK(isCarrierByObservation(1, 0));
    CHECK(isCarrierByObservation(1, 1));
    CHECK_FALSE(isCarrierByObservation(1, 2));
    CHECK(isCarrierByObservation(1, 3));

    // AL=2: 前半CON=0(直列、キャリア=op1) + 後半CON=1(並列、キャリア=op2,op3)
    // 旧実装は後半ペアのみ(0x8)を見ており、op1がキャリア扱いされない
    // バグがあった。
    CHECK_FALSE(isCarrierByObservation(2, 0));
    CHECK(isCarrierByObservation(2, 1));
    CHECK(isCarrierByObservation(2, 2));
    CHECK(isCarrierByObservation(2, 3));

    // AL=3: 前半CON=1(並列、キャリア=op0,op1) + 後半CON=1(並列、キャリア=op2,op3)
    CHECK(isCarrierByObservation(3, 0));
    CHECK(isCarrierByObservation(3, 1));
    CHECK(isCarrierByObservation(3, 2));
    CHECK(isCarrierByObservation(3, 3));
}
