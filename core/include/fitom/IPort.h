#pragma once
// fitom/IPort.h
// チップ I/O ポート抽象インターフェース
//
// ─── アドレス規則 ────────────────────────────────────────────────────────────
//
//   IPort::write(addr, data) の addr は 16bit で表現する。
//
//   addr  7:0  … レジスタアドレス (0x00〜0xFF)
//   addr 15:8  … ポート番号 / a_high
//
//   ポート番号の意味はバックエンドによって異なる:
//     FmEnginePort  → FmEngine_Write() の port 引数 (OPN port0/1 等)
//     HWPort        → HWPlugin_Write() の addr 引数 上位バイト (a_high)
//     SplitPort     → 0x000〜0x0FF は port1 へ / 0x100〜0x1FF は port2 へ振り分け
//
// ─── 2 ポートチップの扱い ─────────────────────────────────────────────────
//
//   OPNA / OPN2 / OPL3 などは物理的に 2 つのポートを持つ。
//   チップドライバは addr >= 0x100 のアドレスを「ポート2」として
//   そのまま setReg(0x1XX, data) と書く。
//
//   SplitPort がその振り分けをバックエンド (HW / エミュレーター) に依存せず吸収する。
//
//   HW (SPFM 等):
//     SplitPort → port1: HWPort(slot=N) → a_high=0 (通常アドレス)
//     SplitPort → port2: HWPort(slot=N) → a_high=1 として振り分け
//                        または HWPort(slot=N+1) に別スロットとして接続
//
//   エミュレーター (FmEngine):
//     FmEnginePort が addr 上位バイトを port 引数に渡すため、
//     SplitPort を経由せず 1 ポートで解決できる。

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

namespace fitom {

// ================================================================
//  IPort: 純粋仮想インターフェース
// ================================================================
class IPort {
public:
    virtual ~IPort() = default;

    // ─── 基本 I/O ─────────────────────────────────────────────────
    // addr: 上位バイト = ポート番号、下位バイト = レジスタアドレス
    virtual void    write(uint16_t addr, uint16_t data)    = 0;
    virtual uint8_t read(uint16_t addr)                    = 0;

    // 生バイト書き込み (アドレスオフセットなし / DCSG 等向け)
    virtual void    writeRaw(uint16_t addr, uint16_t data) { write(addr, data); }

    // バースト書き込み (デフォルトは write() の繰り返し)
    virtual void    writeBurst(uint16_t startAddr,
                               const uint8_t* data, std::size_t length);

    // ─── 制御 ─────────────────────────────────────────────────────
    virtual uint8_t status()  { return 0; }
    virtual void    reset()   {}
    virtual void    flush()   {}

    // ─── メタ情報 ─────────────────────────────────────────────────
    virtual std::string getDesc()          { return "IPort"; }
    virtual std::string getInterfaceDesc() { return ""; }
    virtual int         getClock()         { return 0; }
    virtual int         getPanpot()        { return 0; }

    // サブポートアクセス (SplitPort / MappedPort 向け)
    virtual IPort*  getSubPort(int idx)    { return nullptr; }
    virtual int     getPortCount()         { return 1; }
};

// ================================================================
//  OffsetPort: アドレスオフセット付きポート
// ================================================================
class OffsetPort : public IPort {
public:
    OffsetPort(IPort* parent, uint16_t offset)
        : parent_(parent), offset_(offset) {}

    void    write(uint16_t addr, uint16_t data) override {
        if (parent_) parent_->write(addr + offset_, data);
    }
    void    writeRaw(uint16_t addr, uint16_t data) override {
        if (parent_) parent_->writeRaw(addr, data);
    }
    uint8_t read(uint16_t addr) override {
        return parent_ ? parent_->read(addr + offset_) : 0;
    }
    uint8_t status()  override { return parent_ ? parent_->status() : 0; }
    void    reset()   override { if (parent_) parent_->reset(); }
    int     getClock()  override { return parent_ ? parent_->getClock() : 0; }
    int     getPanpot() override { return parent_ ? parent_->getPanpot() : 0; }
    std::string getInterfaceDesc() override {
        return parent_ ? parent_->getInterfaceDesc() : "";
    }
    std::string getDesc() override;

protected:
    IPort*   parent_ = nullptr;
    uint16_t offset_ = 0;
};

// ================================================================
//  SplitPort: 2 ポートチップ用ポート振り分けアダプター
//
//  write(addr, data):
//    addr < 0x100  → port1_->write(addr,         data)
//    addr >= 0x100 → port2_->write(addr & 0x0FF, data)
//                    ※ port2 側は 0x00〜0xFF のレジスタ空間
//
//  これにより OPNA / OPN2 / OPL3 のチップドライバは:
//    setReg(0xA4 + ch, data)   // port1 (ch0-2)
//    setReg(0x1A4 + ch, data)  // port2 (ch3-5)
//  と書くだけで、HW/エミュレーター両方に対応できる。
//
//  エミュレーターバックエンド (FmEnginePort) では:
//    FmEnginePort は addr 上位バイトを port 引数として渡す実装なので、
//    SplitPort を経由しない 1 ポート接続で完結する。
//    (FmEnginePort は 0x100 以上のアドレスを port=1 として解釈する)
// ================================================================
class SplitPort : public IPort {
public:
    // port1: 0x000〜0x0FF 用、port2: 0x100〜0x1FF 用
    SplitPort(IPort* port1, IPort* port2)
        : port1_(port1), port2_(port2) {}

    void write(uint16_t addr, uint16_t data) override {
        if (addr < 0x100) {
            if (port1_) port1_->write(addr, data);
        } else {
            if (port2_) port2_->write(static_cast<uint16_t>(addr & 0x0FF), data);
        }
    }

    uint8_t read(uint16_t addr) override {
        if (addr < 0x100) return port1_ ? port1_->read(addr) : 0;
        return port2_ ? port2_->read(static_cast<uint16_t>(addr & 0x0FF)) : 0;
    }

    void reset() override {
        if (port1_) port1_->reset();
        if (port2_) port2_->reset();
    }

    void flush() override {
        if (port1_) port1_->flush();
        if (port2_) port2_->flush();
    }

    uint8_t status() override {
        return port1_ ? port1_->status() : 0;
    }

    int getClock()  override { return port1_ ? port1_->getClock() : 0; }
    int getPanpot() override { return port1_ ? port1_->getPanpot() : 0; }

    std::string getInterfaceDesc() override {
        return port1_ ? port1_->getInterfaceDesc() : "";
    }

    std::string getDesc() override {
        std::string d1 = port1_ ? port1_->getDesc() : "null";
        std::string d2 = port2_ ? port2_->getDesc() : "null";
        return "Split[" + d1 + " | " + d2 + "]";
    }

    IPort* getSubPort(int idx) override {
        if (idx == 0) return port1_;
        if (idx == 1) return port2_;
        return nullptr;
    }
    int getPortCount() override { return 2; }

    IPort* port1() const { return port1_; }
    IPort* port2() const { return port2_; }

private:
    IPort* port1_ = nullptr;
    IPort* port2_ = nullptr;
};

// ================================================================
//  MappedPort: 複数ポートをアドレス空間にマッピング
// ================================================================
class MappedPort : public IPort {
public:
    MappedPort() = default;
    MappedPort(IPort* pt, uint32_t addr, uint32_t range);

    IPort*   findPort(uint32_t addr);
    int      findIndex(uint32_t addr);
    uint32_t nextAddress() const;
    uint32_t append(IPort* pt, uint32_t size);
    uint32_t map(IPort* pt, uint32_t addr, uint32_t size);

    void    write(uint16_t addr, uint16_t data) override;
    uint8_t read(uint16_t addr)                 override;
    uint8_t status()    override;
    void    reset()     override;
    int     getPanpot() override;
    int     getClock()  override;

    IPort*      getSubPort(int idx)    override;
    int         getPortCount()         override { return static_cast<int>(ports_.size()); }
    std::string getInterfaceDesc()     override;
    std::string getDesc()              override;

protected:
    struct PortMap { IPort* port; uint32_t addr; uint32_t range; };
    std::vector<PortMap> ports_;
};

} // namespace fitom
