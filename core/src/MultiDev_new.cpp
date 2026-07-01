// fitom/MultiDev_new.cpp
// マルチデバイス — ISoundDevice ベース移行版
//
// CMultiDevice: 複数 ISoundDevice を束ねる基底
// CSpanDevice:  ポリフォニーを複数チップに展開する
// CUnison:      全チップを同時発音する (ユニゾン/デチューン)

#include "fitom/ISoundDevice.h"
#include "fitom/Log.h"
#include <vector>
#include <algorithm>

namespace fitom {

// ================================================================
//  CMultiDevice: ISoundDevice を複数束ねる基底
// ================================================================
class CMultiDevice : public ISoundDevice {
public:
    CMultiDevice() = default;

    void addDevice(ISoundDevice* dev) {
        if (dev) chips_.push_back(dev);
    }

    // ─── ISoundDevice 実装 ──────────────────────────────────────────────
    uint8_t     getDeviceType() const override {
        return chips_.empty() ? 0 : chips_[0]->getDeviceType();
    }
    uint8_t     getChCount()    const override {
        uint8_t n = 0;
        for (auto* c : chips_) n += c->getChCount();
        return n;
    }
    IPort*      getPort()       override {
        return chips_.empty() ? nullptr : chips_[0]->getPort();
    }
    std::string getDescriptor() const override {
        if (chips_.empty()) return "MultiDevice(empty)";
        return "Multi[" + chips_[0]->getDescriptor() + " x" +
               std::to_string(chips_.size()) + "]";
    }

    void setReg(uint16_t reg, uint8_t data, bool force = false) override {
        for (auto* c : chips_) c->setReg(reg, data, force);
    }
    uint8_t getReg(uint16_t reg) const override {
        return chips_.empty() ? 0 : chips_[0]->getReg(reg);
    }
    void reset() override { for (auto* c : chips_) c->reset(); }
    void init()  override { for (auto* c : chips_) c->init(); }

    void setMasterVolume(uint8_t vol) override {
        for (auto* c : chips_) c->setMasterVolume(vol);
    }
    void pollingCallback() override {
        for (auto* c : chips_) c->pollingCallback();
    }
    void timerCallback(uint32_t tick) override {
        for (auto* c : chips_) c->timerCallback(tick);
    }

    // チャンネル管理・発音制御はサブクラスに委ねる
    uint8_t allocCh(IMidiCh* o, const HwPatch* p) override  = 0;
    uint8_t assignCh(uint8_t ch, IMidiCh* o, const HwPatch* p) override = 0;
    uint8_t queryCh(IMidiCh* o, const HwPatch* p, int m) override = 0;
    void    releaseCh(uint8_t ch) override = 0;
    void    enableCh(uint8_t ch, bool e) override = 0;
    uint8_t getAvailableChs() const override = 0;
    void    noteOn(uint8_t ch, uint8_t vel) override = 0;
    void    noteOff(uint8_t ch) override = 0;
    bool    isChOwnedBy(uint8_t ch, const IMidiCh* owner) const override = 0;
    void    setVoice(uint8_t ch, const HwPatch& p, bool u) override = 0;
    void    setNoteFine(uint8_t ch, uint8_t note, int16_t fine, bool u) override = 0;
    void    setVolume(uint8_t ch, uint8_t vol, bool u) override = 0;
    void    setVelocity(uint8_t ch, uint8_t vel, bool u) override = 0;
    void    setExpression(uint8_t ch, uint8_t exp, bool u) override = 0;
    void    setPanpot(uint8_t ch, int8_t pan, bool u) override = 0;
    void    setSustain(uint8_t ch, bool sus, bool u) override = 0;
    const HwPatch* getCurrentPatch(uint8_t ch) const override = 0;
    uint8_t        getCurrentNote(uint8_t ch) const override = 0;

protected:
    std::vector<ISoundDevice*> chips_;

    // チャンネル番号 → デバイスとデバイス内チャンネルに分解
    struct DevCh { ISoundDevice* dev; uint8_t ch; };
    DevCh resolveGlobalCh(uint8_t globalCh) const {
        for (auto* c : chips_) {
            if (globalCh < c->getChCount()) return {c, globalCh};
            globalCh -= c->getChCount();
        }
        return {nullptr, 0xFF};
    }
    uint8_t toGlobalCh(ISoundDevice* dev, uint8_t localCh) const {
        uint8_t offset = 0;
        for (auto* c : chips_) {
            if (c == dev) return offset + localCh;
            offset += c->getChCount();
        }
        return 0xFF;
    }
};

// ================================================================
//  CSpanDevice: ポリフォニーを複数チップに展開
//  複数チップを「1つの大きなデバイス」として見せる
// ================================================================
class CSpanDevice : public CMultiDevice {
public:
    CSpanDevice() = default;
    CSpanDevice(ISoundDevice* chip1, ISoundDevice* chip2) {
        addDevice(chip1); addDevice(chip2);
    }

    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        for (auto* c : chips_) {
            uint8_t lch = c->queryCh(owner, patch, mode);
            if (lch != 0xFF) return toGlobalCh(c, lch);
        }
        return 0xFF;
    }

    uint8_t allocCh(IMidiCh* owner, const HwPatch* patch) override {
        uint8_t ret = queryCh(owner, patch, 1);
        if (ret == 0xFF) ret = queryCh(nullptr, patch, 1);
        if (ret == 0xFF) ret = queryCh(nullptr, nullptr, 1);
        if (ret != 0xFF) assignCh(ret, owner, patch);
        return ret;
    }

    uint8_t assignCh(uint8_t gch, IMidiCh* owner, const HwPatch* patch) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (!dev) return 0xFF;
        return (dev->assignCh(lch, owner, patch) != 0xFF) ? gch : 0xFF;
    }

    void releaseCh(uint8_t gch) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (dev) dev->releaseCh(lch);
    }

    void enableCh(uint8_t gch, bool e) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (dev) dev->enableCh(lch, e);
    }

    uint8_t getAvailableChs() const override {
        uint8_t n = 0;
        for (auto* c : chips_) n += c->getAvailableChs();
        return n;
    }

    void noteOn(uint8_t gch, uint8_t vel) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (dev) dev->noteOn(lch, vel);
    }
    void noteOff(uint8_t gch) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (dev) dev->noteOff(lch);
    }
    bool isChOwnedBy(uint8_t gch, const IMidiCh* owner) const override {
        auto [dev, lch] = resolveGlobalCh(gch);
        return dev && dev->isChOwnedBy(lch, owner);
    }

#define SPAN_DELEGATE(fn, ...) { auto [dev, lch] = resolveGlobalCh(ch); if (dev) dev->fn(lch, __VA_ARGS__); }
    void setVoice(uint8_t ch, const HwPatch& p, bool u) override      SPAN_DELEGATE(setVoice, p, u)
    void setNoteFine(uint8_t ch, uint8_t n, int16_t f, bool u) override SPAN_DELEGATE(setNoteFine, n, f, u)
    void setVolume(uint8_t ch, uint8_t v, bool u) override             SPAN_DELEGATE(setVolume, v, u)
    void setVelocity(uint8_t ch, uint8_t v, bool u) override           SPAN_DELEGATE(setVelocity, v, u)
    void setExpression(uint8_t ch, uint8_t e, bool u) override         SPAN_DELEGATE(setExpression, e, u)
    void setPanpot(uint8_t ch, int8_t p, bool u) override              SPAN_DELEGATE(setPanpot, p, u)
    void setSustain(uint8_t ch, bool s, bool u) override               SPAN_DELEGATE(setSustain, s, u)
#undef SPAN_DELEGATE

    const HwPatch* getCurrentPatch(uint8_t gch) const override {
        auto [dev, lch] = resolveGlobalCh(gch);
        return dev ? dev->getCurrentPatch(lch) : nullptr;
    }
    uint8_t getCurrentNote(uint8_t gch) const override {
        auto [dev, lch] = resolveGlobalCh(gch);
        return dev ? dev->getCurrentNote(lch) : 0xFF;
    }
};

// ================================================================
//  CUnison: 全チップを同時発音 (ユニゾン / デチューン)
//  alloc したチャンネルが実は「チップ数分のセット」を表す
//  ch = ユニゾングループインデックス
// ================================================================
class CUnison : public CMultiDevice {
public:
    CUnison() = default;
    CUnison(ISoundDevice* chip1, ISoundDevice* chip2) {
        addDevice(chip1); addDevice(chip2);
    }

    uint8_t getChCount() const override {
        // ユニゾンなので、各チップの ch 数の最小値がグループ数
        if (chips_.empty()) return 0;
        uint8_t n = chips_[0]->getChCount();
        for (auto* c : chips_) n = std::min(n, c->getChCount());
        return n;
    }

    // ch はグループインデックス。全チップの同番チャンネルに同時発音
    uint8_t queryCh(IMidiCh* o, const HwPatch* p, int mode) override {
        for (uint8_t gch = 0; gch < getChCount(); ++gch) {
            bool ok = true;
            for (auto* c : chips_) {
                uint8_t lch = c->queryCh(o, p, mode);
                // 全チップで同番チャンネルが空いている場合のみ OK
                auto* st = dynamic_cast<CSoundDevice*>(c);
                if (!st || !st->getChState(gch) || !st->getChState(gch)->isEmpty()) {
                    ok = false; break;
                }
            }
            if (ok) return gch;
        }
        return 0xFF;
    }

    uint8_t allocCh(IMidiCh* owner, const HwPatch* patch) override {
        uint8_t gch = queryCh(owner, patch, 1);
        if (gch == 0xFF) {
            // 強制奪取: 最も古いグループ
            gch = 0; // 単純化: 常にグループ0から奪う
        }
        assignCh(gch, owner, patch);
        return gch;
    }

    uint8_t assignCh(uint8_t gch, IMidiCh* owner, const HwPatch* patch) override {
        for (auto* c : chips_) c->assignCh(gch, owner, patch);
        return gch;
    }

    void releaseCh(uint8_t gch) override {
        for (auto* c : chips_) c->releaseCh(gch);
    }

    void enableCh(uint8_t gch, bool e) override {
        for (auto* c : chips_) c->enableCh(gch, e);
    }

    uint8_t getAvailableChs() const override {
        if (chips_.empty()) return 0;
        uint8_t n = chips_[0]->getAvailableChs();
        for (auto* c : chips_) n = std::min(n, c->getAvailableChs());
        return n;
    }

    void noteOn(uint8_t gch, uint8_t vel) override {
        int detuneStep = 8; // デチューン量 [cent相当]
        int n = static_cast<int>(chips_.size());
        for (int i = 0; i < n; ++i) {
            // デチューン: 中央から対称に展開
            int16_t detune = static_cast<int16_t>((i - n / 2) * detuneStep);
            auto* csd = dynamic_cast<CSoundDevice*>(chips_[i]);
            if (csd) {
                auto* st = csd->getChState(gch);
                if (st) st->fineFreq = detune;
            }
            chips_[i]->noteOn(gch, vel);
        }
    }

    void noteOff(uint8_t gch) override {
        for (auto* c : chips_) c->noteOff(gch);
    }
    bool isChOwnedBy(uint8_t gch, const IMidiCh* owner) const override {
        // ユニゾングループは全チップが同じ owner を共有する設計のため、
        // 先頭チップの状態を代表として確認する。
        return !chips_.empty() && chips_[0]->isChOwnedBy(gch, owner);
    }

#define UNISON_BROADCAST(fn, ...) { for (auto* c : chips_) c->fn(ch, __VA_ARGS__); }
    void setVoice(uint8_t ch, const HwPatch& p, bool u) override      UNISON_BROADCAST(setVoice, p, u)
    void setNoteFine(uint8_t ch, uint8_t n, int16_t f, bool u) override UNISON_BROADCAST(setNoteFine, n, f, u)
    void setVolume(uint8_t ch, uint8_t v, bool u) override             UNISON_BROADCAST(setVolume, v, u)
    void setVelocity(uint8_t ch, uint8_t v, bool u) override           UNISON_BROADCAST(setVelocity, v, u)
    void setExpression(uint8_t ch, uint8_t e, bool u) override         UNISON_BROADCAST(setExpression, e, u)
    void setPanpot(uint8_t ch, int8_t p, bool u) override              UNISON_BROADCAST(setPanpot, p, u)
    void setSustain(uint8_t ch, bool s, bool u) override               UNISON_BROADCAST(setSustain, s, u)
#undef UNISON_BROADCAST

    const HwPatch* getCurrentPatch(uint8_t gch) const override {
        return chips_.empty() ? nullptr : chips_[0]->getCurrentPatch(gch);
    }
    uint8_t getCurrentNote(uint8_t gch) const override {
        return chips_.empty() ? 0xFF : chips_[0]->getCurrentNote(gch);
    }
};

} // namespace fitom
