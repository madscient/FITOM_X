#include "fitom/SccWaveData.h"
// fitom/PSG_new.cpp
// CSSG / CDCSG / CSCC チップドライバ — ISoundDevice ベース移行版
//
// PSG 系の共通特性:
//   - FnumTableType::SSG (period テーブル) または TonePeriod
//   - 音量は線形 4bit (0=最大, 15=最小 ← OPM/OPN と逆)
//   - TL は 7bit で保持するが、writeReg 時に 4bit に変換
//   - ノイズチャンネルは ALG で選択 (ALG=0:トーン/1:ノイズ/2:両方/3:MIX)
//   - DCSG (SN76489): 非対称アドレス (writeRaw のみ)
//   - SCC: 波形テーブル書き込みが必要
//
// ソフトウェアエンベロープ (SoftEnvelope):
//   PSG 系チップは HW EG (AY-3-8910/YM2149 のチップ内蔵エンベロープ) を
//   持つが機能が限定的なため、通常は AR/DR/SL/SR/RR による
//   ソフトウェア ADSR で音量を時間変化させる。
//   パラメータ範囲・挙動は一般的な FM 音源 (OPN) と同じ設定範囲
//   (AR/DR/SR: 5bit 0-31, SL/RR: 4bit 0-15) を使い、実機 FM 音源特有の
//   「凹型アタックカーブ (乗算的減衰)」「指数的ディケイ/リリース」を
//   ソフトウェアで再現する。旧 FITOM の CEnvelope (線形加算モデル) とは
//   別物として全面的に作り直した。

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/Log.h"
#include "fitom/VolumeUtils.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace fitom {

// ================================================================
//  SoftEnvelope: FM音源 (OPN) 相当の ADSR をソフトウェアで再現する
//
//  値域: 0(最大音量) 〜 127(無音) — FITOM_X の TL 空間と同じ極性。
//  パラメータ範囲は OPN の HwOp と同じ:
//    AR/DR/SR: 0-31 (5bit)   SL/RR: 0-15 (4bit)
//
//  実機 FM 音源の特性を模す2種類のカーブ:
//    ATTACK: 乗算的に 0 へ近づける (凹型カーブ。値が大きいほど速く動き、
//            0 に近づくほど遅くなる。実機の AR 特有の「立ち上がりの粘り」)
//    DECAY/SUSTAIN/RELEASE: 加算的に増加 (指数的減衰 = dB空間で線形)
//
//  レート→速度の対応は「レート+4で速度が概ね2倍になる」という
//  実機 FM 音源でよく知られる特性を近似したテーブルで表現する。
//  ビット単位で実機と一致するものではなく実用上の近似値である。
// ================================================================
class SoftEnvelope {
public:
    enum class Phase { Attack, Decay, Sustain, Release, Stopped };

    // op: ベロシティ補正済みの AR/DR/SL/SR/RR (VoiceProcessor::velAR() 等)
    void start(uint8_t ar, uint8_t dr, uint8_t sl, uint8_t sr, uint8_t rr) noexcept
    {
        ar_ = ar; dr_ = dr; sr_ = sr; rr_ = rr;
        sl_ = slToAttenuation(sl);
        value_ = 127.0f;   // NoteOn 時は無音から立ち上がる
        phase_ = (ar_ > 0) ? Phase::Attack : Phase::Decay;
    }

    void release() noexcept {
        if (phase_ != Phase::Stopped) phase_ = Phase::Release;
    }

    void stop() noexcept {
        phase_ = Phase::Stopped;
        value_ = 127.0f;
    }

    // 1 tick 進める。まだ動作中なら true。
    bool update() noexcept
    {
        switch (phase_) {
        case Phase::Attack:
            if (ar_ == 0) break;   // AR=0: 永遠に立ち上がらない (実機と同じ)
            value_ -= value_ * kAttackCoef[ar_] + kAttackMin[ar_];
            if (value_ <= 0.0f) { value_ = 0.0f; phase_ = Phase::Decay; }
            break;
        case Phase::Decay:
            if (dr_ == 0) break;   // DR=0: ディケイなし (サステインレベルで保持されない = 永久ホールド)
            value_ += kRateStep[dr_];
            if (value_ >= sl_) { value_ = sl_; phase_ = Phase::Sustain; }
            break;
        case Phase::Sustain:
            if (sr_ == 0) break;   // SR=0: サステインレート無効 (無限にホールド)
            value_ += kRateStep[sr_];
            if (value_ >= 127.0f) value_ = 127.0f;
            break;
        case Phase::Release:
            // RR は実機同様 effective rate = RR*2+1 (RR=0 でも僅かに減衰する)
            value_ += kRateStep[std::min(31, rr_ * 2 + 1)];
            if (value_ >= 127.0f) { value_ = 127.0f; phase_ = Phase::Stopped; }
            break;
        case Phase::Stopped:
            return false;
        }
        return phase_ != Phase::Stopped;
    }

    uint8_t getValue() const noexcept {
        return static_cast<uint8_t>(std::clamp(value_, 0.0f, 127.0f));
    }
    Phase phase() const noexcept { return phase_; }

private:
    // SL (0-15) → 減衰量 (0-127, 0.75dB単位)。実機は 3dB/step、SL=15 のみ 93dB。
    static float slToAttenuation(uint8_t sl) noexcept {
        return (sl >= 15) ? 124.0f : static_cast<float>(sl) * 4.0f;
    }

    Phase   phase_ = Phase::Stopped;
    float   value_ = 127.0f;
    uint8_t ar_ = 0, dr_ = 0, sr_ = 0, rr_ = 0;
    float   sl_ = 0.0f;

    // ── レートテーブル (実機FM音源の「レート+4で速度倍増」特性の近似) ──────
    // kRateStep: 加算フェーズ (Decay/Sustain/Release) の1tickあたり増分。
    //   index 0 = 無効(0)、31 = 最速 (127段を約14tickで走破)、
    //   1 = 最遅 (127段を約2500tick=2.5秒で走破)。
    static constexpr float kRateStep[32] = {
        0.0f,
        0.050f, 0.059f, 0.071f, 0.084f, 0.100f, 0.119f, 0.141f, 0.168f,
        0.200f, 0.238f, 0.283f, 0.336f, 0.400f, 0.476f, 0.566f, 0.673f,
        0.800f, 0.951f, 1.131f, 1.345f, 1.600f, 1.903f, 2.263f, 2.691f,
        3.200f, 3.805f, 4.525f, 5.382f, 6.400f, 7.609f, 9.051f
    };
    // kAttackCoef / kAttackMin: 乗算的アタックの係数と最小減衰量。
    // value -= value*coef + min という式で、実機特有の凹型カーブを作る。
    static constexpr float kAttackCoef[32] = {
        0.0f,
        0.005f, 0.0059f, 0.0071f, 0.0084f, 0.0100f, 0.0119f, 0.0141f, 0.0168f,
        0.0200f, 0.0238f, 0.0283f, 0.0336f, 0.0400f, 0.0476f, 0.0566f, 0.0673f,
        0.0800f, 0.0951f, 0.1131f, 0.1345f, 0.1600f, 0.1903f, 0.2263f, 0.2691f,
        0.3200f, 0.3805f, 0.4525f, 0.5382f, 0.6400f, 0.7609f, 0.9051f
    };
    static constexpr float kAttackMin[32] = {
        0.0f,
        0.005f, 0.0059f, 0.0071f, 0.0084f, 0.0100f, 0.0119f, 0.0141f, 0.0168f,
        0.0200f, 0.0238f, 0.0283f, 0.0336f, 0.0400f, 0.0476f, 0.0566f, 0.0673f,
        0.0800f, 0.0951f, 0.1131f, 0.1345f, 0.1600f, 0.1903f, 0.2263f, 0.2691f,
        0.3200f, 0.3805f, 0.4525f, 0.5382f, 0.6400f, 0.7609f, 0.9051f
    };
};

// ================================================================
//  CPSGBase: SSG / PSG 共通基底
// ================================================================
class CPSGBase : public CSoundDevice {
public:
    CPSGBase(uint8_t devId, IPort* port, int regSize,
             uint8_t maxChs, int sampleRate,
             uint8_t numOps = 2,
             int noteOffset = FNUM_OFFSET,
             FnumTableType fnumType = FnumTableType::SSG)
        : CSoundDevice(devId, maxChs, port,
                       sampleRate, 1,
                       noteOffset, fnumType, regSize)
    {
        opCount_ = numOps;
        std::fill(lfoTL_, lfoTL_ + MAX_CHS, 64);
    }

protected:
    uint8_t lfoTL_[MAX_CHS]; // ソフトLFO の基準TL (振幅変調用)
    SoftEnvelope envelopes_[MAX_CHS]; // チャンネルごとのソフトウェアADSR

    // PSG 系共通: キーオン/オフはソフトウェアエンベロープの起動/リリースのみ。
    // トーン/ノイズのミックスレジスタ (reg 0x07) はここでは操作しない
    // (updateVoice で一度だけ設定し、以降は音量が0になることで無音化する。
    //  実機FM音源のキーオフがEGのリリースフェーズに入るだけなのと同じ)。
    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        if (s.hwPatch.hwOp[0].EGT & 0x08) return; // HW EG使用時はチップが自動制御
        if (keyOn) {
            envelopes_[ch].start(s.proc.velAR(0), s.proc.velDR(0),
                                 s.proc.velSL(0), s.proc.velSR(0),
                                 s.proc.velRR(0));
        } else {
            envelopes_[ch].release();
        }
    }

    // 毎tick呼ばれる。ソフトウェアエンベロープを進め、音量レジスタへ反映する。
    // エンベロープが自然終了 (Release完了) したら、既存の Releasing タイマーを
    // 即座に0にして次の tick で解放されるようにする (2000ms固定待ちより早く
    // 正確なタイミングで解放される。2000msは万一の安全網として残る)。
    void timerCallback(uint32_t tick) override {
        for (int ch = 0; ch < maxChs_; ++ch) {
            auto& s = chState_[ch];
            if (s.hwPatch.hwOp[0].EGT & 0x08) continue; // HW EG対象外
            if (envelopes_[ch].phase() != SoftEnvelope::Phase::Stopped) {
                envelopes_[ch].update();
                updateVolExp(static_cast<uint8_t>(ch));
            } else if (s.isReleasing()) {
                s.releaseTimer = 0; // 次の CSoundDevice::timerCallback で free()
            }
        }
        CSoundDevice::timerCallback(tick);
    }

    // vol×exp×vel×patchTL×ソフトウェアエンベロープ×振幅LFO を統合した
    // 最終ラウドネス値 (0-127, 高いほど大きい) を返す。
    // 派生クラスは戻り値を自チップのレジスタ形式に変換するだけでよい。
    uint8_t computeFinalLoudness(uint8_t ch) const {
        const auto& s = chState_[ch];
        uint8_t baseLoudness = fitom::calcVolExpVel(s.volume, s.expression, s.velocity);
        uint8_t withTL = fitom::calcLinearLevel(baseLoudness, s.hwPatch.hwOp[0].TL);

        int totalAtten = static_cast<int>(envelopes_[ch].getValue())
                        + (static_cast<int>(lfoTL_[ch]) - 64);
        totalAtten = std::clamp(totalAtten, 0, 127);

        return fitom::calcLinearLevel(withTL, static_cast<uint8_t>(totalAtten));
    }

    // 振幅LFO: lfoTL_ (共有状態) を更新して仮想 updateVolExp() を呼ぶだけの
    // 汎用実装。updateVolExp は各派生クラスがチップ固有形式で実装するため、
    // ポリモーフィズムにより自然に正しいレジスタへ反映される。
    // (ノイズ周波数LFO等、チップ固有レジスタへの直接書き込みが必要な
    //  ケースは CSSG 側で個別にオーバーライドすること)
    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        if (op == 0) { // 振幅 LFO
            lfoTL_[ch] = lev;
            updateVolExp(ch);
        }
    }

    void updateSustain(uint8_t /*ch*/) override {}
    void updatePanpot(uint8_t /*ch*/) override {}

    // 全PSG系共通の初期化ヘルパー。各派生クラスの updateVoice 先頭で呼ぶ。
    // (以前は CPSGBase::updateVoice に SSG 固有のレジスタ操作
    //  (ミックスレジスタ0x07・ノイズ周波数0x06・HW EG 0x08-0x0D) と
    //  一緒に書かれていたが、これらは AY-3-8910/YM2149 (CSSG) 専用の
    //  レジスタ配置であり、CDCSG/CSCC は updateVoice を完全に
    //  オーバーライドして使っていなかった。CSSG 側に移動した)
    void resetLfoBaseline(uint8_t ch) { lfoTL_[ch] = 64; }
};

// ================================================================
//  CSSG (YM2149 SSG / AY-3-8910)
// ================================================================
class CSSG : public CPSGBase {
public:
    CSSG(IPort* port, int sampleRate, uint8_t devId = DEVICE_SSG)
        : CPSGBase(devId, port, 0x20, 3, sampleRate) {}

    std::string getDescriptor() const override { return "SSG (YM2149) 3ch"; }
    void init() override { setReg(0x07, 0x3F, true); } // 全チャンネル無効化

    void updateKey(uint8_t ch, bool keyOn) override {
        const HwPatch& p = chState_[ch].hwPatch;
        if (p.hwOp[0].EGT & 0x08) {
            // HW EG: キーオンはエンベロープ開始で代用
            if (keyOn) updateVoice(ch);
            else       setReg(static_cast<uint16_t>(0x08 + ch),
                              static_cast<uint8_t>(getReg(static_cast<uint16_t>(0x08 + ch)) & 0xE0), true);
        } else {
            CPSGBase::updateKey(ch, keyOn);
        }
    }

protected:
    // AY-3-8910/YM2149 (SSG) 固有のレジスタ操作。
    // トーン/ノイズのミックス機能(0x07)・HW EG(0x08-0x0D)はSSG専用のハードウェア
    // 機能であり、CDCSG(SN76489)/CSCC(SCC)には存在しないため、CPSGBaseではなく
    // ここ(CSSG)に実装する。
    void updateVoice(uint8_t ch) override {
        resetLfoBaseline(ch);
        const HwPatch& p = chState_[ch].hwPatch;
        uint8_t mixBit = computeMixBit(ch, p);
        uint8_t cur = getReg(0x07) & ~((0x09u) << ch);
        setReg(0x07, static_cast<uint8_t>(cur | (mixBit << ch)), true);

        // ノイズ周波数
        if ((p.hw.ALG & 3) == 1 || (p.hw.ALG & 3) == 2) {
            setReg(0x06, p.hw.NFQ & 0x1F, true);
        }

        // HW エンベロープ (EGT bit3 = 使用フラグ)
        // AY-3-8910 / YM2149: HW EG 使用時はすべてのベロシティ感度を無効化する。
        // HW EG はチップ固有のエンベロープ形状 (0xD = attack+decay 等) で制御され、
        // ソフトウェア側からの EG レート補正は意味をなさないためレジスタ値そのまま。
        //
        // 実機仕様: レジスタ 0x0B(Fine)+0x0C(Coarse) は分割不可の単一16bit
        // 「Envelope Period」値であり、SL/RR/DR/SR のような4分割ADSR
        // パラメータではない。ext.HWEP に16bit値をそのまま指定する。
        if (p.hwOp[0].EGT & 0x08) {
            setReg(static_cast<uint16_t>(0x08 + ch),
                   static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x08 + ch)) & 0xE0)
                   | 0x10 | (p.hwOp[0].EGT & 0xF)), true);
            setReg(0x0B, static_cast<uint8_t>(p.ext.HWEP & 0xFF), true);        // Fine
            setReg(0x0C, static_cast<uint8_t>((p.ext.HWEP >> 8) & 0xFF), true); // Coarse
            setReg(0x0D, static_cast<uint8_t>(p.hwOp[0].EGT & 0xF), true);      // Shape
        }
    }

    // ノイズ/トーン ALG に基づくミックスビット計算
    // ミックスレジスタ bit: トーン=bit[ch], ノイズ=bit[ch+3]
    // 0=有効, 1=無効 (Active Low)
    static uint8_t computeMixBit(uint8_t ch, const HwPatch& p) {
        switch (p.hw.ALG & 3) {
        case 0: return 0x08; // トーンのみ (ノイズ無効ビットを立てる)
        case 1: return 0x01; // ノイズのみ (トーン無効ビットを立てる)
        case 2: return 0x00; // 両方有効
        case 3: return 0x09; // 両方無効 (実質消音)
        default: return 0x09;
        }
    }

    void updateVolExp(uint8_t ch) override {
        if (chState_[ch].hwPatch.hwOp[0].EGT & 0x08) return; // HW EG 使用中
        uint8_t finalLoudness = computeFinalLoudness(ch);
        // PSGネイティブ4bitレジスタへ変換 (48dB/3dBステップ)。
        // AY-3-8910/YM2149 は 0=最大音量, 15=最小音量 (反転極性)。
        uint8_t vol = 15u - fitom::linear2dB(finalLoudness, RANGE48DB, STEP300DB, 4);
        setReg(static_cast<uint16_t>(0x08 + ch), vol & 0x0F, false);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t oct = fnum.block;
        uint16_t etp = fnum.fnum >> (oct + 3);
        setReg(static_cast<uint16_t>(ch * 2),     static_cast<uint8_t>(etp & 0xFF), false);
        setReg(static_cast<uint16_t>(ch * 2 + 1), static_cast<uint8_t>(etp >> 8),   false);
    }

    // 振幅LFOは CPSGBase::updateTL (汎用実装) に任せ、
    // ノイズ周波数LFO (SSG固有、reg 0x06 直書き) のみここで追加処理する。
    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        CPSGBase::updateTL(ch, op, lev);
        if (op == 1) { // ノイズ周波数 LFO
            const HwPatch& p = chState_[ch].hwPatch;
            if ((p.hw.ALG & 3) == 1 || (p.hw.ALG & 3) == 2) {
                int16_t frq = static_cast<int16_t>(lev) - 64
                            + static_cast<int16_t>(p.hw.NFQ);
                frq = std::clamp<int16_t>(frq, 0, 31);
                setReg(0x06, static_cast<uint8_t>(frq), false);
            }
        }
    }

    // ノイズ有効時は ch2 を優先割り当て (SSGは3chのうちch2がノイズと共有)
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (patch && (patch->hw.ALG & 3) != 0) {
            const auto& s2 = chState_[2];
            bool avail = mode ? s2.isEmpty() : s2.isEnabled();
            return avail ? 2 : 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }
};

// ================================================================
//  CDCSG (SN76489 DCSG)
//  レジスタアクセスは writeRaw (アドレスなし、常にポート0)
//  周波数はトーン期間 (TonePeriod)、4ch (tone*3 + noise)
// ================================================================
class CDCSG : public CPSGBase {
public:
    CDCSG(IPort* port, int sampleRate)
        : CPSGBase(DEVICE_DCSG, port, 0x10, 4, sampleRate,
                   2, -576, FnumTableType::TonePeriod)
        , prevNoise_(0)
    {
        std::fill(prevVol_,  prevVol_  + 4, 0u);
        std::fill(prevFreq_, prevFreq_ + 4, 0u);
        // ノイズチャンネル (ch3) は手動割り当てのみ
        chState_[3].autoAssign = false;
    }

    std::string getDescriptor() const override { return "DCSG (SN76489) 4ch"; }

    void init() override {
        for (int ch = 0; ch < 3; ++ch) {
            port_->writeRaw(0, static_cast<uint16_t>(0x80 | (ch * 32)));
            port_->writeRaw(0, 0x00);
            port_->writeRaw(0, static_cast<uint16_t>(0x90 | (ch * 32) | 0xF));
        }
        port_->writeRaw(0, 0xE0);
        port_->writeRaw(0, 0xFF);
    }

protected:
    uint8_t  prevNoise_;
    uint8_t  prevVol_[4];
    uint16_t prevFreq_[4];

    void updateVolExp(uint8_t ch) override {
        uint8_t finalLoudness = computeFinalLoudness(ch);
        // DCSG も逆: 0=最大, 15=最小
        uint8_t vol = 15u - fitom::linear2dB(finalLoudness, RANGE48DB, STEP300DB, 4);
        if (prevVol_[ch] == vol) return;
        prevVol_[ch] = vol;
        if (ch < 3) port_->writeRaw(0, static_cast<uint16_t>(0x90 | (ch * 32) | (vol & 0xF)));
        else        port_->writeRaw(0, static_cast<uint16_t>(0xF0 | (vol & 0xF)));
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        if (ch >= 3) return;
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t oct = fnum.block;
        uint16_t etp = fnum.fnum >> (oct + 3);
        if (prevFreq_[ch] == etp) return;
        prevFreq_[ch] = etp;
        port_->writeRaw(0, static_cast<uint16_t>(0x80 | (ch * 32) | (etp & 0xF)));
        port_->writeRaw(0, static_cast<uint16_t>((etp >> 4) & 0x3F));
    }

    void updateVoice(uint8_t ch) override {
        resetLfoBaseline(ch);
        if (ch == 3) {
            const HwPatch& p = chState_[3].hwPatch;
            if (p.hw.ALG == 1) {
                prevNoise_ = static_cast<uint8_t>(0xE0 | ((p.hw.FB & 1) << 2) | (p.hw.NFQ & 3));
                port_->writeRaw(0, prevNoise_);
            }
        }
        // 音量は updateVolExp で書く
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        // DCSG はキーオン/オフレジスタを持たないため音量0で代用するが、
        // ソフトウェアエンベロープの起動/リリースは基底クラスに任せる。
        CPSGBase::updateKey(ch, keyOn);
    }

    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (patch && patch->hw.ALG == 1) {
            const auto& s3 = chState_[3];
            return (mode ? s3.isEmpty() : s3.isEnabled()) ? 3 : 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }
};

// ================================================================
//  CSCC (SCC / SCC+)
//  SCC は 5ch、各チャンネルに 32 バイトの波形テーブルがある
//
//  B-1 対応:
//    FmHwOp::WS = 波形番号 (0〜127) → SccWaveRegistry から波形を引く
//    updateVoice(ch) が WS を見て setWaveform(ch, data) を呼ぶ
//    SccWaveRegistry のポインタを setWaveRegistry() で注入する
// ================================================================
class CSCC : public CPSGBase {
public:
    CSCC(IPort* port, int sampleRate, uint8_t devId = DEVICE_SCC)
        : CPSGBase(devId, port, 0xB0, 5, sampleRate,
                   2, FNUM_OFFSET, FnumTableType::TonePeriod)
        , waveReg_(nullptr)
    {}

    std::string getDescriptor() const override { return "SCC (K051649) 5ch"; }

    // PatchManager から SccWaveRegistry を注入する
    void setWaveRegistry(const SccWaveRegistry* reg) override { waveReg_ = reg; }
    // SCC Wave Bank 番号 (デフォルト 0)
    void setWaveBankNo(int bankNo) { waveBankNo_ = bankNo; }

    void init() override {
        // 全チャンネル無効・波形テーブルをデフォルト(矩形波)で初期化
        setReg(0xAA, 0x00, true);
        // WS=0 の矩形波を全チャンネルに書き込む
        for (uint8_t ch = 0; ch < 5; ++ch) {
            const int8_t* data = waveReg_
                ? waveReg_->getWaveData(waveBankNo_, 0)
                : SccWaveBank{}.getWaveData(0);
            writeWaveform(ch, data);
        }
    }

protected:
    const SccWaveRegistry* waveReg_     = nullptr;
    int                    waveBankNo_  = 0;
    // 直前に書き込んだ WS 番号（同じ波形の再書き込みをスキップ）
    uint8_t prevWS_[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // チップへの波形書き込み（内部使用）
    void writeWaveform(uint8_t ch, const int8_t* data) {
        uint16_t base = static_cast<uint16_t>(ch * 0x20);
        for (int i = 0; i < SCC_WAVE_SIZE; ++i) {
            setReg(static_cast<uint16_t>(base + i),
                   static_cast<uint8_t>(data[i]), true);
        }
    }

    void updateVoice(uint8_t ch) override {
        if (ch >= 5) return;
        resetLfoBaseline(ch);
        const auto& p  = chState_[ch].hwPatch;
        // ch に対応するオペレータの WS を波形番号として使う
        // SCC は ch ごとに独立した波形を持つので hwOp[0].WS を使う
        uint8_t ws = p.hwOp[0].WS & 0x7F;  // 7bit (0〜127)

        // 実機制約: 無印SCC(K051649)はch3とch4が波形メモリを物理的に共有する
        // (SCC+では解消済み)。ch4への書き込みはch3のメモリを上書きしてしまう
        // ため、queryCh()がch3と同一波形の場合のみch4を割り当てる前提で、
        // ch4への実際のレジスタ書き込みはスキップする(ch3が既に正しい波形を
        // 書き込み済みのため、ch4は何もしなくてもch3のデータをそのまま使う)。
        if (deviceType_ == DEVICE_SCC && ch == 4) {
            prevWS_[4] = ws;
            return;
        }

        if (ws != prevWS_[ch]) {
            const int8_t* data = waveReg_
                ? waveReg_->getWaveData(waveBankNo_, ws)
                : SccWaveBank{}.getWaveData(ws);
            writeWaveform(ch, data);
            prevWS_[ch] = ws;
        }
    }

    // 実機制約 (無印SCCのみ): ch3/ch4は波形メモリを共有するため、
    // ch3が既に確保している波形と完全に一致する場合のみch4を割り当てる。
    // SCC+ (DEVICE_SCCP) はこの制約が解消されているため通常の空きch検索を使う。
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (deviceType_ != DEVICE_SCC) {
            return CSoundDevice::queryCh(owner, patch, mode);
        }
        // ch0-2は制約なし、通常通り空きを探す
        for (uint8_t ch = 0; ch < 3; ++ch) {
            bool avail = mode ? chState_[ch].isEmpty() : chState_[ch].isEnabled();
            if (avail) return ch;
        }
        // ch3が空いていれば優先的にch3を使う
        {
            bool avail = mode ? chState_[3].isEmpty() : chState_[3].isEnabled();
            if (avail) return 3;
        }
        // ch3が使用中で、かつ要求された波形がch3のものと完全一致する場合のみ
        // ch4を候補にする (波形メモリ共有のため、異なる波形では使えない)
        if (patch && chState_[3].isActive()) {
            uint8_t requestedWS = patch->hwOp[0].WS & 0x7F;
            uint8_t ch3WS = chState_[3].hwPatch.hwOp[0].WS & 0x7F;
            if (requestedWS == ch3WS) {
                bool avail = mode ? chState_[4].isEmpty() : chState_[4].isEnabled();
                if (avail) return 4;
            }
        }
        return 0xFF;
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // SCC: 0xA0+ch*2 = period LSB, 0xA1+ch*2 = period MSB (12bit)
        uint16_t period = fnum.fnum >> (fnum.block + 3);
        setReg(static_cast<uint16_t>(0xA0 + ch * 2),
               static_cast<uint8_t>(period & 0xFF), false);
        setReg(static_cast<uint16_t>(0xA1 + ch * 2),
               static_cast<uint8_t>((period >> 8) & 0xF), false);
    }

    void updateVolExp(uint8_t ch) override {
        uint8_t finalLoudness = computeFinalLoudness(ch);
        // SCC は正極性 (0=最小, 15=最大)。48dB/3dBステップは他PSG系と共通。
        uint8_t atten = fitom::linear2dB(finalLoudness, RANGE48DB, STEP300DB, 4);
        uint8_t vol   = 15u - atten;
        setReg(static_cast<uint16_t>(0xA8 + ch), vol & 0xF, false);
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        // ソフトウェアエンベロープの起動/リリースは基底クラスに任せる
        CPSGBase::updateKey(ch, keyOn);
        uint8_t cur = getReg(0xAA);
        if (keyOn) cur |=  (1u << ch);
        else       cur &= ~(1u << ch);
        setReg(0xAA, cur, true);
    }
};

// ================================================================
//  CSAA1099 (Philips SAA1099) — 6ch、矩形波+2系統ノイズ+2系統HWエンベロープ
//
//  実機データシート確認済み。旧FITOM(SAA.cpp)はSCCWAVE波形テーブルを
//  誤って流用しており、実機とは異なる不完全な実装だったため全面新規実装する。
//
//  レジスタマップ (実機確定):
//    0x00-0x05: 振幅 (ch0-5)。bit0-3=左音量(4bit)、bit4-7=右音量(4bit)
//    0x08-0x0D: 周波数 (ch0-5)、8bit (値が大きいほど低音)
//    0x10-0x12: オクターブ (3bit×2ch/レジスタ、ch0-5)
//    0x14: トーン有効ビット (bit0-5)
//    0x15: ノイズ有効ビット (bit0-5)
//    0x16: ノイズパラメータ (bit0-1=ノイズ0[ch0-2共有], bit4-5=ノイズ1[ch3-5共有])
//    0x18/0x19: エンベロープ0/1パラメータ (ch0-2/ch3-5共有、8波形)
//    0x1C: 全チャンネル有効(bit0)
//
//  波形はデューティ50%固定の矩形波のみ (SCC相当の波形メモリ機能は無い)。
//
//  HWエンベロープはch0-2/ch3-5の3ch単位で共有されるハードウェア制約があるため、
//  「音色データがデバイスを選択する」原則に従い、queryChが該当グループの
//  空きchを返すのみでモデル化する (専用フィールド不要)。既に同一エンベロープ
//  設定で発音中のchがあるグループを優先することで、レジスタ競合を避ける。
// ================================================================
class CSAA1099 : public CPSGBase {
public:
    // masterClock: 実機は7.15909MHz(GameBlaster)〜8MHz程度が一般的
    CSAA1099(IPort* port, int sampleRate, int masterClock = 8000000)
        : CPSGBase(DEVICE_SAA, port, 0x20, 6, masterClock, 1, 0, FnumTableType::None)
    {}

    std::string getDescriptor() const override { return "SAA1099 6ch"; }

    void init() override {
        setReg(0x1C, 0x01, true); // 全チャンネル有効 (Sync&Resetは立てない)
    }

    // HWエンベロープ使用時、既に同一設定(ext.HWEP下位6bit)で発音中のchが
    // あるグループ(0-2 or 3-5)を優先する。レジスタ0x18/0x19は3ch単位で
    // 共有されるため、異なる設定が同じグループに混在すると後勝ちで
    // 上書きされてしまう問題を、可能な限り同一設定のchをまとめることで回避する。
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (!patch || !(patch->hwOp[0].EGT & 0x08)) {
            return CSoundDevice::queryCh(owner, patch, mode);
        }
        uint8_t requestedEnv = patch->ext.HWEP & 0x3F;

        // 同一エンベロープ設定で既に発音中のchを含むグループを優先
        int preferredBase = -1;
        for (uint8_t base : {uint8_t{0}, uint8_t{3}}) {
            for (uint8_t ch = base; ch < base + 3; ++ch) {
                const auto& s = chState_[ch];
                if (s.isActive() && (s.hwPatch.hwOp[0].EGT & 0x08)
                    && (s.hwPatch.ext.HWEP & 0x3F) == requestedEnv) {
                    preferredBase = base;
                    break;
                }
            }
            if (preferredBase >= 0) break;
        }

        auto findInGroup = [&](uint8_t base) -> uint8_t {
            for (uint8_t ch = base; ch < base + 3; ++ch) {
                bool avail = mode ? chState_[ch].isEmpty() : chState_[ch].isEnabled();
                if (avail) return ch;
            }
            return 0xFF;
        };

        if (preferredBase >= 0) {
            uint8_t r = findInGroup(static_cast<uint8_t>(preferredBase));
            if (r != 0xFF) return r;
        }
        // フォールバック: 任意の空きグループ (0-2優先)
        uint8_t r = findInGroup(0);
        if (r != 0xFF) return r;
        return findInGroup(3);
    }

protected:
    void updateVoice(uint8_t ch) override {
        const HwPatch& p = chState_[ch].hwPatch;

        // トーン/ノイズ有効ビット (CSSGのALG意味論を流用: 0=トーン/1=ノイズ/2=両方)
        bool toneOn  = (p.hw.ALG & 3) != 1;
        bool noiseOn = (p.hw.ALG & 3) != 0;
        uint8_t toneCur  = getReg(0x14) & static_cast<uint8_t>(~(1u << ch));
        uint8_t noiseCur = getReg(0x15) & static_cast<uint8_t>(~(1u << ch));
        setReg(0x14, static_cast<uint8_t>(toneCur  | (toneOn  ? (1u << ch) : 0)));
        setReg(0x15, static_cast<uint8_t>(noiseCur | (noiseOn ? (1u << ch) : 0)));

        // ノイズパラメータ (2系統: ch0-2→bit0-1、ch3-5→bit4-5)
        if (noiseOn) {
            uint8_t nfq = p.hw.NFQ & 0x3;
            bool isGroup1 = (ch >= 3);
            uint8_t cur = getReg(0x16) & static_cast<uint8_t>(isGroup1 ? 0x0F : 0xF0);
            setReg(0x16, static_cast<uint8_t>(cur | (isGroup1 ? (nfq << 4) : nfq)));
        }

        // HWエンベロープ (EGT bit3 = 使用フラグ、ext.HWEP下位6bitにパラメータ)
        // bit0-2:mode(波形0-7), bit3:resolution, bit4:clockSrc, bit5:rightInvert
        if (p.hwOp[0].EGT & 0x08) {
            uint16_t envReg = (ch < 3) ? 0x18 : 0x19;
            uint8_t hwep = p.ext.HWEP & 0x3F;
            uint8_t mode         = hwep & 0x7;
            uint8_t resolution   = (hwep >> 3) & 1;
            uint8_t clockSrc     = (hwep >> 4) & 1;
            uint8_t rightInvert  = (hwep >> 5) & 1;
            setReg(envReg, static_cast<uint8_t>(0x80 | rightInvert | (mode << 1)
                                                 | (resolution << 4) | (clockSrc << 5)));
        }

        updateVolExp(ch);
    }

    // ステレオ音量: 等パワーパンニング (CLinearPanDeviceと同じ式)。
    // HWエンベロープ使用時は振幅レジスタをエンベロープが制御するため書かない。
    void updateVolExp(uint8_t ch) override {
        if (chState_[ch].hwPatch.hwOp[0].EGT & 0x08) return;
        const auto& s = chState_[ch];
        int p = std::clamp(static_cast<int>(s.panpot) + 64 - 1, 0, 126);
        double lgain = std::cos(M_PI_2 * p / 126.0);
        double rgain = std::sin(M_PI_2 * p / 126.0);
        uint8_t loudness = computeFinalLoudness(ch); // 0-127
        uint8_t lvol4 = static_cast<uint8_t>(std::lround(lgain * loudness * 15.0 / 127.0));
        uint8_t rvol4 = static_cast<uint8_t>(std::lround(rgain * loudness * 15.0 / 127.0));
        setReg(static_cast<uint16_t>(0x00 + ch),
               static_cast<uint8_t>(((rvol4 & 0xF) << 4) | (lvol4 & 0xF)), false);
    }

    // 周波数: 実機式 freq = (2×masterClock/512) × 2^octave / (511 - freqReg) を
    // freqReg について解き、0-255に収まるoctaveを探索する。
    // (Fnum.cpp の GetSAATP は本チップの実機式と一致しない可能性があるため、
    //  ここで直接計算する)
    void updateFreq(uint8_t ch, const ChState::Fnum* /*fn*/) override {
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return;

        double semitone = static_cast<double>(s.lastNote) - 69.0
                         + static_cast<double>(s.fineFreq) / 64.0 / 100.0 * 1.0;
        double hz = 440.0 * std::pow(2.0, semitone / 12.0);
        if (hz <= 0.0) return;

        double base = 2.0 * fnumMaster_ / 512.0;
        int oct = 0;
        double freqRegF = 0.0;
        for (oct = 0; oct <= 7; ++oct) {
            freqRegF = 511.0 - (base * std::pow(2.0, oct)) / hz;
            if (freqRegF >= 0.0 && freqRegF <= 255.0) break;
        }
        freqRegF = std::clamp(freqRegF, 0.0, 255.0);
        uint8_t freqReg = static_cast<uint8_t>(std::lround(freqRegF));

        setReg(static_cast<uint16_t>(0x08 + ch), freqReg, false);

        uint16_t octReg = static_cast<uint16_t>(0x10 + ch / 2);
        bool highNibble = (ch % 2) == 1;
        uint8_t cur = getReg(octReg) & static_cast<uint8_t>(highNibble ? 0x0F : 0xF0);
        uint8_t shifted = static_cast<uint8_t>(highNibble ? (oct << 4) : oct);
        setReg(octReg, static_cast<uint8_t>(cur | shifted), false);
    }

    void updatePanpot(uint8_t ch) override { updateVolExp(ch); }
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCSSG(IPort* p, int sr)  { return std::make_unique<CSSG>(p, sr); }
std::unique_ptr<ISoundDevice> createCDCSG(IPort* p, int sr) { return std::make_unique<CDCSG>(p, sr); }
std::unique_ptr<ISoundDevice> createCSCC(IPort* p, int sr, uint32_t deviceType) {
    return std::make_unique<CSCC>(p, sr, static_cast<uint8_t>(deviceType));
}
std::unique_ptr<ISoundDevice> createCSAA1099(IPort* p, int sr) { return std::make_unique<CSAA1099>(p, sr); }
} // namespace fitom
