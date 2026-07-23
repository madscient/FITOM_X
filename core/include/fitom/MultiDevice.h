// fitom/MultiDevice.h
// マルチデバイス基底クラス群 — ヘッダー化
//
// CMultiDevice: 複数 ISoundDevice を束ねる基底
// CSpanDevice:  ポリフォニーを複数チップに展開する（例: OPN2/OPNA/OPL3 の
//               2ポート構成を「1つのNchデバイス」として見せる）
// CUnison:      全チップを同時発音する (ユニゾン/デチューン)
//
// 個別チップドライバ (OPN2_new.cpp, OPL_new.cpp 等) が CSpanDevice を
// 継承してサブチップを束ねられるよう、実装をヘッダーに公開する。
// (旧実装は MultiDev_new.cpp 内の .cpp ローカルクラスとしてのみ存在しており、
//  他の翻訳単位から継承できない状態だった)

#pragma once

#include "fitom/ISoundDevice.h"
#include "fitom/PcmBankData.h"
#include <vector>
#include <algorithm>
#include <string>
#include <cmath>

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

    // 特定チップのみ有効なオプショナルインターフェース (SCC波形テーブル /
    // ADPCM系PCMバンク) は、ISoundDevice基底のデフォルトがno-opのため、
    // CMultiDeviceが未オーバーライドのままだと同種デバイス自動束ね
    // (CSpanDevice、VoicePatchType基準)やCUnison経由の構成で、実際の
    // 発音を担うサブチップ側に一切伝播しない(2026年7月、ADPCM-Aが
    // 2チップにspanされた構成で「キーオンは動くが波形アドレスが常に0の
    // まま」という不具合の根本原因として発覚。setPcmRegistry()/
    // initPcmData()がCSpanDevice自身のno-opデフォルトで止まっており、
    // 束ねられた実チップ(CAdPcm2610A等)のvoices_テーブルが一度も
    // 登録されていなかった)。全サブチップへブロードキャストする。
    void setWaveRegistry(const SccWaveRegistry* reg) override {
        for (auto* c : chips_) c->setWaveRegistry(reg);
    }
    // bankNoは呼び出し元(CFITOM::initDevices())が代表デバイス(chips_[0])の
    // deviceTypeから解決した値だが、束ねられたサブチップが異なる物理チップ
    // (例: OPNA用ADPCM-BとOPNB/OPNBB用ADPCM-Bが同一VoicePatchTypeで束ねられる
    // 構成)の場合、正しいPCMバンク(波形バイナリのバウンダリ整列がチップ毎に
    // 異なる)はサブチップ毎に違う。そのため各サブチップについて、まず
    // そのサブチップ自身のdeviceTypeに完全一致するバンクを個別に探し
    // (PcmBankRegistry::findBankNoForDeviceType()、PcmBank::deviceTypeの
    // コメント参照)、見つかった場合はbankNoではなくそちらを使う。見つから
    // ない場合のみ、代表デバイス基準で解決済みのbankNoにフォールバックする
    // (単一チップ種のみが束ねられている旧来の構成との後方互換)。
    // 2026-07-24、OPNA+OPNB+OPNBBのADPCM-Bが束ねられた構成で、OPNB/OPNBB
    // 側のサブチップにもbankNo(代表=OPNAのバンク)がそのまま伝播しており、
    // OPNB/OPNBB用の正しいバンクが一切反映されないバグとして発覚。
    void setPcmRegistry(const PcmBankRegistry* reg, int bankNo = 0) override {
        for (auto* c : chips_) {
            int chipBankNo = bankNo;
            if (reg) {
                int specific = reg->findBankNoForDeviceType(c->getDeviceType());
                if (specific >= 0) chipBankNo = specific;
            }
            c->setPcmRegistry(reg, chipBankNo);
        }
    }
    void initPcmData() override {
        for (auto* c : chips_) c->initPcmData();
    }
    void onMasterPitchChanged(double pitchHz) override {
        for (auto* c : chips_) c->onMasterPitchChanged(pitchHz);
    }
    void pollingCallback() override {
        for (auto* c : chips_) c->pollingCallback();
    }
    void timerCallback(uint32_t tick) override {
        for (auto* c : chips_) c->timerCallback(tick);
    }

    // チャンネル管理・発音制御はサブクラスに委ねる
    uint8_t allocCh(IMidiCh* o, const HwPatch* p, uint8_t vel,
                     const SwPatch* sw = nullptr,
                     const SampleZonePatch* sp = nullptr) override  = 0;
    uint8_t assignCh(uint8_t ch, IMidiCh* o, const HwPatch* p, uint8_t vel,
                      const SwPatch* sw = nullptr,
                      const SampleZonePatch* sp = nullptr) override = 0;
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
    void    setCC1Modulation(uint8_t ch, uint8_t cc1, int16_t maxDepth) override = 0;
    void    setLfoDepthOverride(uint8_t ch, int16_t cents) override = 0;
    void    updateTL(uint8_t ch, uint8_t op, uint8_t tl) override = 0;

    // グローバルch → 実チップのローカルchへ変換して ChState を返す
    ChState* getChState(uint8_t ch) override {
        auto dc = resolveGlobalCh(ch);
        return dc.dev ? dc.dev->getChState(dc.ch) : nullptr;
    }
    const ChState* getChState(uint8_t ch) const override {
        auto dc = resolveGlobalCh(ch);
        return dc.dev ? dc.dev->getChState(dc.ch) : nullptr;
    }

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

    // 単体 CSoundDevice::allocCh (findBestCh) と同じ設計思想:
    // patch は一切 nullptr にせず (デバイス制約情報を最後まで保持する)、
    // mode だけを 1(奪取なし)→0(奪取あり) の順に緩和して再試行する。
    // (旧実装は owner/patch を順に nullptr化する3段階リトライだったが、
    //  3回ともmode=1のままで強制奪取が機能せず、かつ最終段でpatch=nullptr
    //  になることでデバイス制約(例: ノイズ→ch7固定)を最も必要な場面
    //  (強制奪取判断時)で失うという重大な欠陥があった)
    uint8_t allocCh(IMidiCh* owner, const HwPatch* patch, uint8_t vel,
                     const SwPatch* swPatch = nullptr,
                     const SampleZonePatch* samplePatch = nullptr) override {
        for (int mode : {1, 0}) {
            for (auto* c : chips_) {
                uint8_t lch = c->queryCh(owner, patch, mode);
                if (lch != 0xFF) {
                    uint8_t gch = toGlobalCh(c, lch);
                    assignCh(gch, owner, patch, vel, swPatch, samplePatch);
                    return gch;
                }
            }
        }
        return 0xFF;
    }

    uint8_t assignCh(uint8_t gch, IMidiCh* owner, const HwPatch* patch, uint8_t vel,
                      const SwPatch* swPatch = nullptr,
                      const SampleZonePatch* samplePatch = nullptr) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (!dev) return 0xFF;
        return (dev->assignCh(lch, owner, patch, vel, swPatch, samplePatch) != 0xFF) ? gch : 0xFF;
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
    // forceDamp は ISoundDevice のデフォルト実装 (単純な noteOff) では
    // サブチップ本来の急速減衰処理 (RR最大化等) がスキップされてしまうため、
    // 明示的に委譲する。
    void forceDamp(uint8_t gch) override {
        auto [dev, lch] = resolveGlobalCh(gch);
        if (dev) dev->forceDamp(lch);
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
    // HW LFO: OPM/OPZ等がスパン(複数物理チップを1つのデバイスとして
    // 束ねる構成)されている場合、Depth/Rateは物理チップごとに独立した
    // リソースのため、そのチャンネルが実際に発音している物理チップへ
    // 正しく転送する必要がある(2026年7月追加。以前はCMultiDevice/
    // CSpanDeviceがこれらのメソッドを一切オーバーライドしておらず、
    // ISoundDevice基底のno-opにフォールバックしていたため、スパン構成
    // ではHW LFOが常に無効化されたままだった)。
    void enablePM(uint8_t ch, bool on) override                        SPAN_DELEGATE(enablePM, on)
    void enableAM(uint8_t ch, bool on) override                        SPAN_DELEGATE(enableAM, on)
    void setLFODepth(uint8_t ch, uint8_t dep) override                 SPAN_DELEGATE(setLFODepth, dep)
    void setLFORate(uint8_t ch, uint8_t rate) override                 SPAN_DELEGATE(setLFORate, rate)
#undef SPAN_DELEGATE

    void setCC1Modulation(uint8_t ch, uint8_t cc1, int16_t maxDepth) override {
        auto [dev, lch] = resolveGlobalCh(ch);
        if (dev) dev->setCC1Modulation(lch, cc1, maxDepth);
    }
    void setLfoDepthOverride(uint8_t ch, int16_t cents) override {
        auto [dev, lch] = resolveGlobalCh(ch);
        if (dev) dev->setLfoDepthOverride(lch, cents);
    }
    void updateTL(uint8_t ch, uint8_t op, uint8_t tl) override {
        auto [dev, lch] = resolveGlobalCh(ch);
        if (dev) dev->updateTL(lch, op, tl);
    }

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
    // 代表チップ(chips_[0])に queryCh を尋ね、その戻り値 (デバイス制約を
    // 反映したch番号、または制約を満たせない場合は0xFF) をそのまま採用する。
    // CUnison は同一デバイス種別のチップのみを束ねる前提のため、
    // 代表チップの制約判定は他の全チップにもそのまま当てはまる。
    // (修正前は各チップのqueryCh戻り値を完全に無視し、gch=0から単純な
    //  空きチェックのみで判定していたため、OPMノイズ(ch7固定)等の
    //  デバイス制約付き音色がCUnison経由で正しくch7に割り当てられない
    //  重大なバグがあった)
    uint8_t queryCh(IMidiCh* o, const HwPatch* p, int mode) override {
        if (chips_.empty()) return 0xFF;
        uint8_t gch = chips_[0]->queryCh(o, p, mode);
        if (gch == 0xFF) return 0xFF;
        // 代表チップ以外も同じgchが実際に空いているか再確認
        for (auto* c : chips_) {
            const auto* st = c->getChState(gch);
            if (!st || !st->isEmpty()) return 0xFF;
        }
        return gch;
    }

    // 単体 CSoundDevice::allocCh と同じ設計思想:
    // patch は一切 nullptr にせず、mode だけを 1(奪取なし)→0(奪取あり)
    // の順に緩和して再試行する。queryCh (代表チップ方式、修正済み) が
    // デバイス制約を正しく反映するため、その戻り値をそのまま使う。
    // 旧実装の「見つからなければ強制的にグループ0」というフォールバックは、
    // mode=0 (強制奪取許可) まで正しく試行すればほぼ起こり得ないため削除した。
    uint8_t allocCh(IMidiCh* owner, const HwPatch* patch, uint8_t vel,
                     const SwPatch* swPatch = nullptr,
                     const SampleZonePatch* samplePatch = nullptr) override {
        for (int mode : {1, 0}) {
            uint8_t gch = queryCh(owner, patch, mode);
            if (gch != 0xFF) {
                assignCh(gch, owner, patch, vel, swPatch, samplePatch);
                return gch;
            }
        }
        return 0xFF;
    }

    uint8_t assignCh(uint8_t gch, IMidiCh* owner, const HwPatch* patch, uint8_t vel,
                      const SwPatch* swPatch = nullptr,
                      const SampleZonePatch* samplePatch = nullptr) override {
        for (auto* c : chips_) c->assignCh(gch, owner, patch, vel, swPatch, samplePatch);
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
            auto* st = chips_[i]->getChState(gch);
            if (st) st->fineFreq = detune;
            chips_[i]->noteOn(gch, vel);
        }
    }

    void noteOff(uint8_t gch) override {
        for (auto* c : chips_) c->noteOff(gch);
    }
    void forceDamp(uint8_t gch) override {
        for (auto* c : chips_) c->forceDamp(gch);
    }
    bool isChOwnedBy(uint8_t gch, const IMidiCh* owner) const override {
        // ユニゾングループは全チップが同じ owner を共有する設計のため、
        // 先頭チップの状態を代表として確認する。
        return !chips_.empty() && chips_[0]->isChOwnedBy(gch, owner);
    }

    // CUnison は「gch = 全チップ共通のローカルch番号」という設計のため、
    // CMultiDevice::getChState (resolveGlobalCh 前提) は使えない。
    // isChOwnedBy と同様、先頭チップの状態を代表として返す。
    ChState* getChState(uint8_t gch) override {
        return chips_.empty() ? nullptr : chips_[0]->getChState(gch);
    }
    const ChState* getChState(uint8_t gch) const override {
        return chips_.empty() ? nullptr : chips_[0]->getChState(gch);
    }

#define UNISON_BROADCAST(fn, ...) { for (auto* c : chips_) c->fn(ch, __VA_ARGS__); }
    void setVoice(uint8_t ch, const HwPatch& p, bool u) override      UNISON_BROADCAST(setVoice, p, u)
    void setNoteFine(uint8_t ch, uint8_t n, int16_t f, bool u) override UNISON_BROADCAST(setNoteFine, n, f, u)
    void setVolume(uint8_t ch, uint8_t v, bool u) override             UNISON_BROADCAST(setVolume, v, u)
    void setVelocity(uint8_t ch, uint8_t v, bool u) override           UNISON_BROADCAST(setVelocity, v, u)
    void setExpression(uint8_t ch, uint8_t e, bool u) override         UNISON_BROADCAST(setExpression, e, u)
    void setPanpot(uint8_t ch, int8_t p, bool u) override              UNISON_BROADCAST(setPanpot, p, u)
    void setSustain(uint8_t ch, bool s, bool u) override               UNISON_BROADCAST(setSustain, s, u)
    // HW LFO: ユニゾン構成は全チップが同じchで同時発音するため、
    // 単純に全チップへ同じ値をブロードキャストする(2026年7月追加、
    // 理由はCSpanDevice側の同種コメント参照)。
    void enablePM(uint8_t ch, bool on) override                        UNISON_BROADCAST(enablePM, on)
    void enableAM(uint8_t ch, bool on) override                        UNISON_BROADCAST(enableAM, on)
    void setLFODepth(uint8_t ch, uint8_t dep) override                 UNISON_BROADCAST(setLFODepth, dep)
    void setLFORate(uint8_t ch, uint8_t rate) override                 UNISON_BROADCAST(setLFORate, rate)
#undef UNISON_BROADCAST

    void setCC1Modulation(uint8_t ch, uint8_t cc1, int16_t maxDepth) override {
        for (auto* c : chips_) c->setCC1Modulation(ch, cc1, maxDepth);
    }
    void setLfoDepthOverride(uint8_t ch, int16_t cents) override {
        for (auto* c : chips_) c->setLfoDepthOverride(ch, cents);
    }
    void updateTL(uint8_t ch, uint8_t op, uint8_t tl) override {
        for (auto* c : chips_) c->updateTL(ch, op, tl);
    }

    const HwPatch* getCurrentPatch(uint8_t gch) const override {
        return chips_.empty() ? nullptr : chips_[0]->getCurrentPatch(gch);
    }
    uint8_t getCurrentNote(uint8_t gch) const override {
        return chips_.empty() ? 0xFF : chips_[0]->getCurrentNote(gch);
    }
};

// ================================================================
//  CLinearPanDevice: 物理的にL/Rに固定配線された同一チップ2台を
//  1つのステレオデバイスとして束ねる (旧FITOM CLinearPan 完全移植)。
//
//  CUnison (両チップに同一chを同時発音) をベースに、setVolume/setPanpot を
//  オーバーライドし、パンポットに応じて等パワーパンニング(cos/sin)で
//  左右チップの音量をクロスフェードする。両チップは常に同時に鳴り続け、
//  「左右どちらか一方だけを鳴らす」という切り替えは行わない。
//
//  ChState (volume/panpot) を保持する仕組みが CMultiDevice 側にないため、
//  本クラス自身が「本来の (計算前の) 音量・パン値」を保持する。
// ================================================================
class CLinearPanDevice : public CUnison {
public:
    // leftChip: Panpot=1(L)側の物理チップ、rightChip: Panpot=2(R)側
    CLinearPanDevice(ISoundDevice* leftChip, ISoundDevice* rightChip)
        : CUnison(leftChip, rightChip)
        // getChCount()(=chips_の最小ch数)分だけ確保する。以前は
        // CSoundDevice::MAX_CHSとは無関係に独自定義した固定長16の
        // 配列だったため、16chを超えるチップ(OPL4 AWM = 24ch)が将来
        // CLinearPanDeviceで束ねられた場合、chState_と同種の範囲外
        // アクセスが再発しうる作りだった(2026年7月、chState_のvector化
        // に合わせて同じ問題を修正)。
        , masterVolume_(getChCount(), uint8_t{127})
        , masterPan_(getChCount(), int8_t{0})
    {}

    void setVolume(uint8_t ch, uint8_t vol, bool update) override {
        if (ch >= masterVolume_.size() || chips_.size() < 2) return;
        masterVolume_[ch] = vol;
        applyLinearPan(ch, update);
    }

    void setPanpot(uint8_t ch, int8_t pan, bool update) override {
        if (ch >= masterPan_.size() || chips_.size() < 2) return;
        masterPan_[ch] = pan;
        applyLinearPan(ch, update);
    }

private:
    std::vector<uint8_t> masterVolume_;
    std::vector<int8_t>  masterPan_;

    // 旧FITOM CLinearPan::UpdatePanpot 完全移植。
    // panpot(int8_t, -64..63) を旧FITOMの0-127生値スケールに変換した上で、
    // 等パワーパンニングの式 (lgain=cos, rgain=sin) を適用する。
    void applyLinearPan(uint8_t ch, bool update) {
        int p = static_cast<int>(masterPan_[ch]) + 64 - 1; // -64..63 → -1..126
        p = std::clamp(p, 0, 126);
        // M_PI_2 は POSIX/GNU拡張でありMSVC標準では未定義のため、
        // 移植性のためリテラル値 (π/2) を直接使う。
        constexpr double kHalfPi = 1.5707963267948966;
        double lgain = std::cos(kHalfPi * p / 126.0);
        double rgain = std::sin(kHalfPi * p / 126.0);
        uint8_t lvol = static_cast<uint8_t>(std::lround(lgain * masterVolume_[ch]));
        uint8_t rvol = static_cast<uint8_t>(std::lround(rgain * masterVolume_[ch]));
        chips_[0]->setVolume(ch, lvol, update); // L
        chips_[1]->setVolume(ch, rvol, update); // R
    }
};

} // namespace fitom
