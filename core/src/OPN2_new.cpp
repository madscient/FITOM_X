// fitom/OPN2_new.cpp
// COPNA / COPN2 チップドライバ — CSpanDevice を使った正しい継承構造
//
// 設計方針:
//   旧FITOMと同様に、OPN2/OPNA は CSpanDevice を継承し、
//   内部に2つの COPN サブチップ (各3ch) を保持する。
//
//   ch 0-2: chip1_ (port1: アドレス 0x000-0x0FF)
//   ch 3-5: chip2_ (port2: アドレス 0x100-0x1FF)
//
// ポート構成:
//   エミュレーター (単一 IPort): chip2_ は OffsetPort(port, 0x100) を使用。
//   SPFM 等の2スロット構成 (2つの IPort): chip2_ は extraPort を使用。
//
// 0x28 レジスタ (キーオン/オフ) の特別扱い:
//   0x28 は「ポート1専用のグローバルレジスタ」であり、
//   ポート2チャンネルはビット2 (0x04) で区別する。
//   OffsetPort でアドレスをずらしただけでは正しく動かないため、
//   OPN2Port2 ラッパーが 0x28 をインターセプトして正しく変換する:
//     chip1->setReg(0x28, data) → port1->write(0x28, data)          (ポート1)
//     chip2->setReg(0x28, data) → port1->write(0x28, data | 0x04)   (ポート2フラグ付き)

#include "fitom/ISoundDevice.h"
#include "fitom/MultiDevice.h"
#include "fitom/IPort.h"
#include "fitom/FITOMdefine.h"
#include "fitom/Log.h"

namespace fitom {

// COPN は OPN_new.cpp で定義されているが、ここから使う
class COPN;
std::unique_ptr<ISoundDevice> createSubCOPN(IPort* port, int fnumMaster);

// ================================================================
//  OPN2Port2: OPN2/OPNA ポート2専用のポートラッパー
//
//  register 0x28 (キーオン/オフ) はポート1専用のグローバルレジスタ。
//  ポート2チャンネルを制御するにはビット2 (0x04) を立てた上で
//  ポート1に書く必要がある。それ以外のレジスタは OffsetPort で
//  +0x100 オフセットを加えてデータポートに書く。
//  ハードウェアの2スロット構成では masterPort_=port1, dataPort_=port2
//  が独立した IPort になる。
// ================================================================
class OPN2Port2 : public IPort {
public:
    // masterPort: 0x28 の書き込み先 (常にポート1)
    // dataPort:   FMパラメータレジスタの書き込み先 (OffsetPort か物理ポート2)
    OPN2Port2(IPort* masterPort, IPort* dataPort) noexcept
        : masterPort_(masterPort), dataPort_(dataPort) {}

    void write(uint16_t addr, uint16_t data) override {
        if (addr == 0x28) {
            // キーオン/オフ: ポート2フラグ (bit2) を立ててポート1に書く
            if (masterPort_)
                masterPort_->write(0x28, static_cast<uint16_t>(data | 0x04));
        } else {
            if (dataPort_) dataPort_->write(addr, data);
        }
    }
    void writeRaw(uint16_t addr, uint16_t data) override {
        if (dataPort_) dataPort_->writeRaw(addr, data);
    }
    uint8_t read(uint16_t addr) override {
        return dataPort_ ? dataPort_->read(addr) : 0;
    }
    uint8_t status()  override { return masterPort_ ? masterPort_->status() : 0; }
    void    reset()   override {}
    int     getClock()   override { return masterPort_ ? masterPort_->getClock() : 0; }
    int     getPanpot()  override { return masterPort_ ? masterPort_->getPanpot() : 0; }
    std::string getInterfaceDesc() override {
        return masterPort_ ? masterPort_->getInterfaceDesc() : "";
    }

private:
    IPort* masterPort_;
    IPort* dataPort_;
};

// ================================================================
//  COPNA: YM2608 (OPNA) / OPN2系 の共通基底
//
//  CSpanDevice を継承し、内部に2つの COPN サブチップを保持する。
//  ch 0-2 → chip1_ (port1)
//  ch 3-5 → chip2_ (OPN2Port2 経由)
// ================================================================
class COPNA : public CSpanDevice {
public:
    // port1: 必須。エミュレーターの場合はこれ1つだけ渡す。
    // port2: SPFM 等の2スロット構成で2番目の IPort がある場合に渡す。
    //        nullptr の場合は内部で OffsetPort(port1, 0x100) を生成する。
    // fnumMaster: チップのマスタークロック [Hz]。OPN2/OPNA 系は 8MHz。
    COPNA(IPort* port1, IPort* port2 = nullptr,
          uint8_t deviceId = DEVICE_OPNA, int fnumMaster = 8000000)
    {
        // エミュレーター (単一ポート) の場合はオフセットポートを生成
        if (!port2) {
            offsetPort_ = std::make_unique<OffsetPort>(port1, 0x100);
            port2 = offsetPort_.get();
        }
        // OPN2Port2: 0x28 をインターセプトし、それ以外は port2 に転送
        port2Wrapper_ = std::make_unique<OPN2Port2>(port1, port2);

        // 2つの COPN サブチップを生成
        chip1_ = createSubCOPN(port1,              fnumMaster);
        chip2_ = createSubCOPN(port2Wrapper_.get(), fnumMaster);

        addDevice(chip1_.get()); // ch 0-2
        addDevice(chip2_.get()); // ch 3-5

        deviceId_ = deviceId;
    }

    uint8_t     getDeviceType() const override { return deviceId_; }
    std::string getDescriptor() const override { return "OPNA (YM2608) 6ch"; }

    void init() override {
        // サブチップをリセット (FMレジスタの初期化)
        chip1_->reset();
        chip2_->reset();

        // ポート1専用グローバルレジスタ (chip1 = port1 経由で書く)
        chip1_->setReg(0x27, 0x30, true); // ch3 通常モード, タイマー停止
        chip1_->setReg(0x29, 0x80, true); // FM enable (OPNA: port1 のみ)
        chip1_->setReg(0x22, 0x00, true); // HW LFO 無効 (ソフトLFO使用のため)
        chip1_->setReg(0x10, 0xBF, true); // ADPCM-B: BRDY割り込み有効
        chip1_->setReg(0x10, 0x00, true); // ADPCM-B: リセット

        // ポート2タイマーモード (chip2 → OPN2Port2 → port2:0x127 で書かれる)
        chip2_->setReg(0x27, 0x00, true);
    }

    void reset() override {
        CMultiDevice::reset();
    }

protected:
    uint8_t deviceId_ = DEVICE_OPNA;

private:
    std::unique_ptr<ISoundDevice> chip1_;        // ch 0-2 (port1)
    std::unique_ptr<ISoundDevice> chip2_;        // ch 3-5 (port2 via OPN2Port2)
    std::unique_ptr<OffsetPort>   offsetPort_;   // エミュレーター用 +0x100 オフセット
    std::unique_ptr<OPN2Port2>    port2Wrapper_; // 0x28 インターセプター
};

// ================================================================
//  COPN2: YM2612 / YM3438 / YMF276 系
//
//  COPNA から派生し、ADPCM/FM enable 等 OPNA 固有の初期化を省く。
//  基本的な6ch FM 動作のみ。
// ================================================================
class COPN2 : public COPNA {
public:
    COPN2(IPort* port1, IPort* port2 = nullptr,
          uint8_t deviceId = DEVICE_OPN2)
        : COPNA(port1, port2, deviceId, 8000000)
    {}

    std::string getDescriptor() const override { return "OPN2 (YM2612) 6ch"; }

    void init() override {
        // OPNA と異なり 0x29 FM enable / ADPCM-B は持たない
        chip1_ref()->reset();
        chip2_ref()->reset();
        chip1_ref()->setReg(0x27, 0x00, true); // タイマー停止 (port1)
        chip2_ref()->setReg(0x27, 0x00, true); // タイマー停止 (port2)
        // YM2612 固有: DAC 無効化
        chip1_ref()->setReg(0x2B, 0x00, true); // DAC disable
    }

private:
    // CSpanDevice の chips_[0] と chips_[1] への参照を返すヘルパー
    ISoundDevice* chip1_ref() { return chips_.size() > 0 ? chips_[0] : nullptr; }
    ISoundDevice* chip2_ref() { return chips_.size() > 1 ? chips_[1] : nullptr; }
};

// ================================================================
//  ファクトリ関数
// ================================================================

// OPN_new.cpp で定義される COPN サブチップ生成関数
// (ヘッダ経由ではなくリンク時解決)
extern std::unique_ptr<ISoundDevice> createSubCOPN_impl(IPort* port, int fnumMaster);
std::unique_ptr<ISoundDevice> createSubCOPN(IPort* port, int fnumMaster) {
    return createSubCOPN_impl(port, fnumMaster);
}

std::unique_ptr<ISoundDevice> createCOPNA(IPort* p, int /*sr*/, IPort* p2) {
    return std::make_unique<COPNA>(p, p2, DEVICE_OPNA);
}
std::unique_ptr<ISoundDevice> createCOPN2(IPort* p, int /*sr*/, IPort* p2) {
    return std::make_unique<COPN2>(p, p2, DEVICE_OPN2);
}
std::unique_ptr<ISoundDevice> createCOPN2C(IPort* p, int /*sr*/, IPort* p2) {
    return std::make_unique<COPN2>(p, p2, DEVICE_OPN2C);
}
std::unique_ptr<ISoundDevice> createCOPN2L(IPort* p, int /*sr*/, IPort* p2) {
    return std::make_unique<COPN2>(p, p2, DEVICE_OPN2L);
}

} // namespace fitom
