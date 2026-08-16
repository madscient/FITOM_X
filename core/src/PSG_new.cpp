#include "fitom/SccWaveData.h"
// fitom/PSG_new.cpp
// CSSG / CSSGS / CSSGS3 / CDCSG / CSCC チップドライバ — ISoundDevice ベース移行版
//
// PSG 系の共通特性:
//   - 周波数レジスタは「周期」(FnumTableType::SSG / TonePeriod)。音程が
//     上がるほど値が小さくなるため、F-number 系とはスケーリングが逆になる
//     (CPSGBase::getFnumber() / tonePeriod() 参照)
//   - 音量レジスタの性質はチップごとに異なり、共通化できない:
//       SSG/EPSG  対数DAC (3dB/step、EPSGは5bitで1.5dB/step)、0=無音
//       DCSG      対数の減衰量 (2dB/step、0-14=0〜-28dB、15=消音)
//       SCC/SAA   リニア乗算 (0=無音、最大値=フルスケール)
//     対数のものだけ linear2dB() を通し、リニアのものは線形にスケールする
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
        // chState_と同じ理由(2026年7月のOPL4 AWM範囲外アクセス修正)で、
        // maxChs_と同数だけ確保するvectorにしている。固定長配列だと、
        // 将来maxChs_がその配列の決め打ちサイズを超えるPSG系チップが
        // 追加された場合に同種のバグを再発しうる。
        lfoTL_.assign(maxChs_, 64);
        envelopes_.resize(maxChs_);
    }

protected:
    std::vector<uint8_t> lfoTL_; // ソフトLFO の基準TL (振幅変調用)
    std::vector<SoftEnvelope> envelopes_; // チャンネルごとのソフトウェアADSR

    // ── 周期テーブル (FnumTableType::SSG / TonePeriod) の扱い ───────────────
    // PSG系の周波数レジスタは「周期」であり、音程が上がるほど値が小さくなる。
    // F-number (音程が上がるほど値が大きくなる) 系チップとは極性も値域も
    // 逆であるため、基底クラスの正規化・スケーリングをそのままは使えない。

    // 周期テーブルが表す1オクターブ分が、getFnumber()のオクターブ番号で
    // 言うどの位置に当たるか。テーブルの生成基点 (noteOffset_、通常
    // FNUM_OFFSET) とテーブルの基準ピッチ (masterPitch_ = A4 = MIDIノート69)
    // の位置関係だけで決まるため、FNUM_OFFSETを変えても追従する。
    int periodBaseOct() const { return (noteOffset_ + 69 * 64) / 768; }

    // 周期テーブルの値を、実際のノートのオクターブへスケーリングして
    // チップのレジスタ値に変換する。chipShift はチップ固有の分周比差分
    // (テーブルの分周比に対し、周期が2倍なら-1、1/2なら+1)。
    uint16_t tonePeriod(const ChState::Fnum& fn, uint16_t mask,
                        int chipShift = 0) const
    {
        int sh = static_cast<int>(fn.block) - periodBaseOct() + chipShift;
        uint32_t v = fn.fnum;
        if (sh >= 0) {
            v = (sh < 32) ? (v >> sh) : 0u;
        } else {
            v = (-sh < 16) ? (v << -sh) : 0xFFFFu;
        }
        return static_cast<uint16_t>(std::min<uint32_t>(v, mask));
    }

    // 周期テーブル専用の F-number 算出。基底実装は11bit F-number前提の
    // 正規化 (bit11が立っていたら右シフトしてblockを繰り上げる / block>7を
    // クランプして不足分をfnumの左シフトで補う) を行うが、これを周期値に
    // 適用すると周期そのものが壊れる。ここではテーブル値とオクターブを
    // 素通しし、実際のスケーリングは tonePeriod() に任せる。
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override
    {
        ChState::Fnum ret;
        if (ch >= maxChs_) return ret;
        const auto& s = chState_[ch];
        if (!fnumTable_ || s.lastNote >= 128) {
            ret.block = static_cast<uint8_t>(static_cast<uint16_t>(s.fineFreq) >> 12);
            ret.fnum  = static_cast<uint16_t>(s.fineFreq) & 0x0FFF;
            return ret;
        }
        int16_t lfoOffset = s.proc.channelLfoActive() ? s.proc.channelLfoValue() : 0;
        int32_t index = static_cast<int32_t>(s.lastNote) * 64
                      + static_cast<int32_t>(s.fineFreq) + offset + lfoOffset;
        int oct = 0;
        while (index < 0)    { --oct; index += 768; }
        while (index >= 768) { ++oct; index -= 768; }
        ret.fnum  = fnumTable_[index];
        ret.block = static_cast<uint8_t>(std::clamp(oct, 0, 31));
        return ret;
    }

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
    //
    // 戻り値を fitom::linear2dB() へ渡すときの step は必ず STEP075DB
    // (=0x7F、実質マスクなし) にすること。linear2dB() は最終シフト
    // (7-range-bw) でレジスタ幅相当のdBステップへ変換するため、step引数で
    // 事前にマスクすると入力の上位bitが失われ、レンジ内で音量が折り返して
    // しまう (STEP300DBだと0-127が0-31へ折り返し、最大音量でもレジスタ値が
    // 半分以下に留まり、ラウドネスが32の倍数付近では逆に完全無音になる)。
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
        : CSSG(port, sampleRate, devId, 3, 0x20) {}

    std::string getDescriptor() const override { return "SSG (YM2149) 3ch"; }

    // 全チャンネル無効化。ユニット数ぶん繰り返す (CSSGS 参照)。
    void init() override {
        for (uint8_t ch = 0; ch < maxChs_; ch += kChsPerUnit)
            setReg(mixReg(ch), 0x3F, true);
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        const HwPatch& p = chState_[ch].hwPatch;
        if (p.hwOp[0].EGT & 0x08) {
            // HW EG: キーオンはエンベロープ開始で代用
            if (keyOn) updateVoice(ch);
            else       setReg(volReg(ch),
                              static_cast<uint8_t>(getReg(volReg(ch)) & 0xE0), true);
        } else {
            CPSGBase::updateKey(ch, keyOn);
        }
    }

protected:
    // YM2149 相当の SSG ブロックを複数内蔵するチップ (YMZ705/SSGS) を、
    // レジスタ空間を 0x20 ごとに繰り返す「ユニット」として同じドライバで
    // 扱えるようにするための索引。単体の YM2149 (maxChs_=3) では
    // unitBase() が常に 0・unitCh() が ch そのものになるため、レジスタ
    // アドレスは従来と1バイトも変わらない。
    static constexpr uint8_t  kChsPerUnit  = 3;
    static constexpr uint16_t kUnitRegSize = 0x20;

    static uint16_t unitBase(uint8_t ch) {
        return static_cast<uint16_t>((ch / kChsPerUnit) * kUnitRegSize);
    }
    static uint8_t unitCh(uint8_t ch) {
        return static_cast<uint8_t>(ch % kChsPerUnit);
    }
    static uint8_t unitOf(uint8_t ch) {
        return static_cast<uint8_t>(ch / kChsPerUnit);
    }

    // ── レジスタアドレス索引 ────────────────────────────────────────────
    // 既定は YM2149 と同じユニット内配置 (unitBase(ch) + 固定オフセット)。
    // ビットフィールドの意味は同じでもアドレスの並べ方だけが違うチップ
    // (YMZ771/SSGS3 は機能ごとにまとめ直されている) は、これらを
    // 差し替えるだけで対応できる。
    virtual uint16_t toneReg(uint8_t ch) const {    // +1 が Coarse
        return static_cast<uint16_t>(unitBase(ch) + unitCh(ch) * 2);
    }
    virtual uint16_t noiseReg(uint8_t ch) const {   // ノイズ周期 (ユニット共有)
        return static_cast<uint16_t>(unitBase(ch) + 0x06);
    }
    virtual uint16_t mixReg(uint8_t ch) const {     // ミックス (ユニット共有)
        return static_cast<uint16_t>(unitBase(ch) + 0x07);
    }
    // 音量レジスタ。HW EG 使用フラグ (bit4) も同居する。
    virtual uint16_t volReg(uint8_t ch) const {
        return static_cast<uint16_t>(unitBase(ch) + 0x08 + unitCh(ch));
    }
    virtual uint16_t envPeriodReg(uint8_t ch) const {  // +1 が Coarse、ユニット共有
        return static_cast<uint16_t>(unitBase(ch) + 0x0B);
    }
    virtual uint16_t envShapeReg(uint8_t ch) const {   // ユニット共有
        return static_cast<uint16_t>(unitBase(ch) + 0x0D);
    }

    CSSG(IPort* port, int sampleRate, uint8_t devId, uint8_t maxChs, int regSize)
        : CPSGBase(devId, port, regSize, maxChs, sampleRate) {}

    // AY-3-8910/YM2149 (SSG) 固有のレジスタ操作。
    // トーン/ノイズのミックス機能(0x07)・HW EG(0x08-0x0D)はSSG専用のハードウェア
    // 機能であり、CDCSG(SN76489)/CSCC(SCC)には存在しないため、CPSGBaseではなく
    // ここ(CSSG)に実装する。
    void updateVoice(uint8_t ch) override {
        resetLfoBaseline(ch);
        const HwPatch& p = chState_[ch].hwPatch;
        const uint8_t  sub  = unitCh(ch);
        uint8_t mixBit = computeMixBit(p);
        uint8_t cur = getReg(mixReg(ch)) & ~((0x09u) << sub);
        setReg(mixReg(ch), static_cast<uint8_t>(cur | (mixBit << sub)), true);

        // ノイズ周波数 (ノイズ発生器はユニット内3chで共有)
        if ((p.hw.ALG & 3) == 1 || (p.hw.ALG & 3) == 2) {
            setReg(noiseReg(ch), p.hw.NFQ & 0x1F, true);
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
            setReg(volReg(ch),
                   static_cast<uint8_t>((getReg(volReg(ch)) & 0xE0)
                   | 0x10 | (p.hwOp[0].EGT & 0xF)), true);
            setReg(envPeriodReg(ch), static_cast<uint8_t>(p.ext.HWEP & 0xFF), true);                            // Fine
            setReg(static_cast<uint16_t>(envPeriodReg(ch) + 1),
                   static_cast<uint8_t>((p.ext.HWEP >> 8) & 0xFF), true);                                      // Coarse
            setReg(envShapeReg(ch), static_cast<uint8_t>(p.hwOp[0].EGT & 0xF), true);                          // Shape
        }
    }

    // ノイズ/トーン ALG に基づくミックスビット計算
    // ミックスレジスタ bit: トーン=bit[ch], ノイズ=bit[ch+3]
    // 0=有効, 1=無効 (Active Low)
    static uint8_t computeMixBit(const HwPatch& p) {
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
        // AY-3-8910/YM2149 の Amplitude は 0=無音, 15=最大音量なので、
        // linear2dB() が返す減衰量を15から引いて音量値に直す。
        uint8_t vol = 15u - fitom::linear2dB(finalLoudness, RANGE48DB, STEP075DB, 4);
        setReg(volReg(ch), vol & 0x0F, false);
    }

    // トーン周期は12bit (0x00-0x05: Fine 8bit + Coarse 4bit)。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint16_t etp = tonePeriod(fnum, 0x0FFF);
        const uint16_t reg = toneReg(ch);
        setReg(reg,                            static_cast<uint8_t>(etp & 0xFF), false);
        setReg(static_cast<uint16_t>(reg + 1), static_cast<uint8_t>(etp >> 8),   false);
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
                setReg(noiseReg(ch), static_cast<uint8_t>(frq), false);
            }
        }
    }

    // ノイズ有効時は各ユニットの最終ch (単体SSGならch2) を優先割り当て
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (patch && (patch->hw.ALG & 3) != 0) {
            for (uint8_t ch = kChsPerUnit - 1; ch < maxChs_; ch += kChsPerUnit) {
                const auto& s = chState_[ch];
                bool avail = mode ? s.isEmpty() : s.isEnabled();
                if (avail) return ch;
            }
            return 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }
};

// ================================================================
//  CSSGS (YMZ705 SSGS) — SSG 互換部 (YM2149 相当 × 2 ユニット = 6ch)
//
//  YMZ732 (SSGS2) も同じクラスが担当する (DEVICE_SSGS2)。SSG部・ADPCM部とも
//  レジスタマップは YMZ705 と完全に同一で、違いはマスタークロックの分周比
//  (DeviceFactory 側で吸収) と、FITOM が使わないシンプルアクセスモード
//  (/SEL ピンでデータバスのみからシーケンサを叩くハードウェアモード) の
//  有無だけ。
//
//  データシート「SSG synthesis control register map」注記のとおり、
//  $00-$0D が SSG-1、$20-$2D が SSG-2 で、いずれも YM2149 とレジスタ
//  互換。加えて各チャンネルに 4bit のパンポットを持つ:
//    $10-$12  CH 1A-1C パンポット
//    $30-$32  CH 2A-2C パンポット
//  【YMZ705 データシートの誤記】register map の $28-$2A は
//  "Channel-2x Pan-pot"、$30-$32 は "Channel-2x Frequency" と書かれているが、
//  ビットフィールド (M/L3-L0 と P3-P0) と「$20-$2D は YM2149 互換」という
//  同ページの注記から明らかに逆。上位互換の YMZ732 データシートでは
//  $28-$2A=音量 / $30-$32=パンポットと正しく記載されており、こちらが正しい。
//
//  音色データ形式・ソフトウェアADSR・ノイズ/HW EG の扱いはすべて単体の
//  YM2149 (CSSG) と同一のため、VoicePatchType も VOICE_PATCH_SSG を共有し、
//  CSSG をそのまま継承してユニット索引とパンポットだけを足す。
//
//  シーケンサ/ミックスレベルのレジスタ ($F0-$F8) には書き込まない。
//  FITOM は内蔵シーケンサを使わず CPU から直接ドライブする構成であり、
//  $F8 (SSG-ADPCM ミックスレベル) はデータシートに 4bit 値と L/R レベルの
//  対応が記載されておらず、増減どちらの向きかを確定できないため、
//  リセット既定値のままにしておく。
// ================================================================
class CSSGS : public CSSG {
public:
    // ssgBlockClock: SSG ブロックの動作クロック (実チップでは 2.048MHz 固定)。
    // マスタークロックからの換算は DeviceFactory 側で行う
    // (SSGS: 4.096MHz÷2 または 6.144MHz÷3 / SSGS2: 12.288MHz÷6)。
    CSSGS(IPort* port, int ssgBlockClock, uint8_t devId = DEVICE_SSGS)
        : CSSG(port, ssgBlockClock, devId, 6, 0x40) {}

    std::string getDescriptor() const override {
        return (deviceType_ == DEVICE_SSGS2) ? "SSGS2 (YMZ732) SSG 6ch"
                                             : "SSGS (YMZ705) SSG 6ch";
    }

    // パンポットレジスタのリセット値0は「中央」ではなく左端 (EPSGemuEngine の
    // ymz_ssg_device::pan_gain 参照) のため、基底の全ch無効化に加えて中央値を
    // 明示的に書く。CSoundDevice::noteOn() は panpot が既定値0のままだと
    // panDirty が立たず updatePanpot() を一度も呼ばないので、ここで書いて
    // おかないとパンCCを送らない限り全チャンネルが左に張り付く。
    void init() override {
        CSSG::init();
        for (uint8_t ch = 0; ch < maxChs_; ++ch)
            setReg(panReg(ch), panCenter(), true);
    }

protected:
    // パンポットの最大値。0=左端 / 中央値=中央 / 最大値=右端。
    // SSGS/SSGS2 は4bit(0-15、中央8)、SSGS3 は5bit(0-31、中央16)。
    // 値と L/R レベルの対応はデータシートに記載が無く、いずれも同世代・
    // 同ファミリの YMZ280B と同じ分配則で解釈する (EPSGemuEngine の
    // ymz_ssg_device::pan_gain と対)。
    virtual uint8_t panMax() const { return 15; }
    uint8_t panCenter() const { return static_cast<uint8_t>((panMax() + 1) / 2); }

    virtual uint16_t panReg(uint8_t ch) const {
        return static_cast<uint16_t>(unitBase(ch) + 0x10 + unitCh(ch));
    }

    void updatePanpot(uint8_t ch) override {
        // -64..+63 を 0..panMax() へ。中央 (0) がちょうど中央値になるよう
        // 四捨五入する (切り捨てだと1段ぶん左へ寄る)。
        const int pan = static_cast<int>(chState_[ch].panpot) + 64;
        const int v = std::clamp(
            static_cast<int>(std::lround(pan * panMax() / 127.0)), 0, static_cast<int>(panMax()));
        setReg(panReg(ch), static_cast<uint8_t>(v), false);
    }
};

// ================================================================
//  CSSGS3 (YMZ771 SSGS3) — SSG 互換部 (6ch)
//
//  SSG 音源としての機能は SSGS/SSGS2 と全く同じで、違いは3点だけ:
//   1. レジスタが「ユニットごと」ではなく「機能ごと」にまとめ直され、
//      $10-$32 の連続した空間に並んでいる (下表)。ビットフィールドの
//      意味は YM2149 と同一のため、アドレス索引を差し替えるだけでよい。
//   2. パンポットが 4bit から 5bit (0-31、16=中央) に拡張されている。
//   3. SSG 全体のトータルボリューム ($32) が追加されている。
//
//    $10-$1B  トーン周期 TP1A/1B/1C/2A/2B/2C (各2バイト、Fine→Coarse)
//    $1C/$1D  ノイズ周期 NP1 / NP2
//    $1E/$1F  ミックス設定 (N*C N*B N*A T*C T*B T*A、YM2149 の $07 と同配置)
//    $20-$25  音量 M/L (1A,1B,1C,2A,2B,2C)
//    $26-$29  エンベロープ周期 EP1(2バイト) / EP2(2バイト)
//    $2A/$2B  エンベロープ形状 (CONT/ATT/ALT/HOLD)
//    $2C-$31  パンポット (1A,1B,1C,2A,2B,2C、5bit)
//    $32      SSG トータルボリューム (128 で 100% の線形ボリューム)
//
//  ADPCM の代わりに搭載された AMM (MPEG Audio 系コーデック) 部は当面
//  対象外のため、SSGS/SSGS2 と違い composite 展開もしない (SSG 部のみの
//  単一デバイスとして構成される)。
// ================================================================
class CSSGS3 : public CSSGS {
public:
    CSSGS3(IPort* port, int ssgBlockClock)
        : CSSGS(port, ssgBlockClock, DEVICE_SSGS3) {}

    std::string getDescriptor() const override { return "SSGS3 (YMZ771) SSG 6ch"; }

    // トータルボリューム ($32) のリセット値は0 = 完全無音。パンポットと
    // 同じく、ここで書いておかないと一切音が出ない。FITOM は音量を
    // チャンネルごとの音量レジスタで作るため、常に 100% で固定する。
    void init() override {
        CSSGS::init();
        setReg(kTotalVolReg, kTotalVolFull, true);
    }

protected:
    static constexpr uint16_t kTotalVolReg  = 0x32;
    static constexpr uint8_t  kTotalVolFull = 128;   // 128 で 100%

    uint8_t panMax() const override { return 31; }

    // 機能ごとにまとめ直されたレジスタ配置 (クラス先頭の表を参照)。
    // 2ユニットが連続して並ぶため、いずれも ch (0-5) から素直に引ける。
    uint16_t toneReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x10 + ch * 2);
    }
    uint16_t noiseReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x1C + unitOf(ch));
    }
    uint16_t mixReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x1E + unitOf(ch));
    }
    uint16_t volReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x20 + ch);
    }
    uint16_t envPeriodReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x26 + unitOf(ch) * 2);
    }
    uint16_t envShapeReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x2A + unitOf(ch));
    }
    uint16_t panReg(uint8_t ch) const override {
        return static_cast<uint16_t>(0x2C + ch);
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
                   2, FNUM_OFFSET, FnumTableType::TonePeriod)
        , prevNoise_(0)
    {
        // init() が全chへ減衰量0xF(消音)を書くため、キャッシュも同じ値で
        // 揃えておかないと最初の「減衰量0xF」の書き込みが抑止されてしまう。
        std::fill(prevVol_,  prevVol_  + 4, 0xFu);
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

    // SN76489 の音量レジスタは「減衰量」であり、0=最大音量・15=無音
    // (init() が 0xF を書いて消音しているのと同じ極性)。刻みは実機仕様の
    // 2dB/step で、0-14 が 0〜-28dB、15 のみ完全消音という狭いレンジしか
    // 持たない。linear2dB() は 0.75dB の2のべき乗倍しか刻めず 2dB を
    // 表現できないため、ここだけは kGM2dB から直接換算する。
    void updateVolExp(uint8_t ch) override {
        uint8_t finalLoudness = computeFinalLoudness(ch);
        int steps = static_cast<int>(std::lround(-fitom::kGM2dB[finalLoudness] / 2.0));
        uint8_t att = static_cast<uint8_t>(std::clamp(steps, 0, 15));
        if (prevVol_[ch] == att) return;
        prevVol_[ch] = att;
        if (ch < 3) port_->writeRaw(0, static_cast<uint16_t>(0x90 | (ch * 32) | (att & 0xF)));
        else        port_->writeRaw(0, static_cast<uint16_t>(0xF0 | (att & 0xF)));
    }

    // トーン周期は10bit (下位4bit + 上位6bit の2バイト転送)。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        if (ch >= 3) return;
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint16_t etp = tonePeriod(fnum, 0x03FF);
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
    // 実機レジスタマップ。無印SCC(K051649)とSCC+(K052539)で波形メモリの
    // 大きさが違う(無印はch3/ch4共有で4ブロック)ため、それ以降の
    // 周波数・音量・チャンネル有効の各レジスタも位置がずれる。
    struct RegMap {
        uint16_t waveform;   // 波形メモリ先頭 (32バイト×ch)
        uint16_t frequency;  // トーン周期 (2バイト×ch)
        uint16_t amplitude;  // 音量 (1バイト×ch)
        uint16_t enable;     // チャンネル有効ビット (bit0-4)
        uint16_t deform;     // 周波数モード・波形ローテーション制御
    };
    // ここに書くのは常に実チップのレジスタ配置であり、エミュレータ
    // ライブラリ独自の番号体系(emu2212の SCC_writeReg 等)へ合わせては
    // ならない。その変換はHW I/Fプラグイン側の責務
    // (docs/plugin-hwif.md「レジスタアドレスは常に実チップのもの」参照)。
    // deformは実機では無印・SCC+とも0xC0に配置される。0xE0はエミュレータ
    // (emu2212)固有のレジスタであり実機には存在しないため書き込まない。
    static constexpr RegMap kRegSCC  = {0x00, 0x80, 0x8A, 0x8F, 0xC0};
    static constexpr RegMap kRegSCCP = {0x00, 0xA0, 0xAA, 0xAF, 0xC0};

    CSCC(IPort* port, int sampleRate, uint8_t devId = DEVICE_SCC)
        : CPSGBase(devId, port, 0xD0, 5, sampleRate,
                   2, FNUM_OFFSET, FnumTableType::TonePeriod)
        , reg_(devId == DEVICE_SCCP ? kRegSCCP : kRegSCC)
        , waveReg_(nullptr)
    {}

    std::string getDescriptor() const override {
        return (deviceType_ == DEVICE_SCCP) ? "SCC+ (K052539) 5ch"
                                            : "SCC (K051649) 5ch";
    }

    // PatchManager から SccWaveRegistry を注入する
    void setWaveRegistry(const SccWaveRegistry* reg) override { waveReg_ = reg; }
    // SCC Wave Bank 番号 (デフォルト 0)
    void setWaveBankNo(int bankNo) { waveBankNo_ = bankNo; }

    void init() override {
        // deform: 8bit/4bit周波数モード・波形ローテーションを全て無効にする。
        // 未初期化のまま残ると周波数レジスタの解釈自体が変わるため、
        // 他のレジスタより先にクリアする。
        setReg(reg_.deform, 0x00, true);
        setReg(reg_.enable, 0x00, true);
        for (uint8_t ch = 0; ch < 5; ++ch) {
            setReg(static_cast<uint16_t>(reg_.amplitude + ch),     0x00, true);
            setReg(static_cast<uint16_t>(reg_.frequency + ch * 2), 0x00, true);
            setReg(static_cast<uint16_t>(reg_.frequency + ch * 2 + 1), 0x00, true);
        }
        // WS=0 の矩形波を全チャンネルに書き込む
        // (無印SCCのch4はch3と波形メモリを共有するため書かない。updateVoice参照)
        for (uint8_t ch = 0; ch < 5; ++ch) {
            if (deviceType_ == DEVICE_SCC && ch == 4) continue;
            const int8_t* data = waveReg_
                ? waveReg_->getWaveData(waveBankNo_, 0)
                : SccWaveBank{}.getWaveData(0);
            writeWaveform(ch, data);
        }
    }

protected:
    const RegMap           reg_;
    const SccWaveRegistry* waveReg_     = nullptr;
    int                    waveBankNo_  = 0;
    // 直前に書き込んだ WS 番号（同じ波形の再書き込みをスキップ）
    uint8_t prevWS_[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    // チップへの波形書き込み（内部使用）
    void writeWaveform(uint8_t ch, const int8_t* data) {
        uint16_t base = static_cast<uint16_t>(reg_.waveform + ch * 0x20);
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

    // トーン周期は12bit (LSB 8bit + MSB 4bit の2バイト)。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint16_t period = tonePeriod(fnum, 0x0FFF);
        setReg(static_cast<uint16_t>(reg_.frequency + ch * 2),
               static_cast<uint8_t>(period & 0xFF), false);
        setReg(static_cast<uint16_t>(reg_.frequency + ch * 2 + 1),
               static_cast<uint8_t>((period >> 8) & 0xF), false);
    }

    // SCC の音量レジスタは波形サンプルへのリニア乗算 (out = wave * vol/15)
    // であり、SSG/DCSG のような対数DACではない。linear2dB() を通すと
    // 意図した減衰量がまったく出ず (例: -24dB のつもりが実際は -6.6dB)、
    // 実効ダイナミックレンジが 0〜-23.5dB に圧縮された上で無音へ落ちる。
    // リニアレジスタを持つ CSAA1099 と同じくラウドネスを線形にスケールする。
    void updateVolExp(uint8_t ch) override {
        uint8_t finalLoudness = computeFinalLoudness(ch);
        uint8_t vol = static_cast<uint8_t>((finalLoudness * 15 + 63) / 127);
        setReg(static_cast<uint16_t>(reg_.amplitude + ch), vol & 0xF, false);
    }

    // キーオンでチャンネル有効ビットを立てるが、キーオフでは落とさない。
    // 実際の消音はソフトウェアエンベロープが音量を0まで下げることで行う
    // (CPSGBase::updateKey のコメント参照)。キーオフで即座に有効ビットを
    // 落とすと、リリースフェーズがまるごと聞こえなくなってしまう。
    void updateKey(uint8_t ch, bool keyOn) override {
        CPSGBase::updateKey(ch, keyOn);
        if (keyOn) {
            setReg(reg_.enable, static_cast<uint8_t>(getReg(reg_.enable) | (1u << ch)), true);
        }
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
// ================================================================
//  CEPSG (AY8930) — AY-3-8910上位互換、Expand Mode常時有効
//
//  旧FITOM CEPSG (EPSG.cpp) 完全移植。AY8930はAY-3-8910/YM2149(CSSG)の
//  上位互換チップで、Expand Mode有効時に以下の拡張機能を持つ:
//    - レジスタ0xd の bit4-5 で Bank A(0xa0)/Bank B(0xb0) を切り替える
//      マルチプレクス方式 (同じレジスタアドレスでもバンクにより意味が変わる)
//    - 5bit音量 (通常のAY-3-8910/YM2149は4bit)
//    - 各chが独立したHWエンベロープを持てる (無印AY-3-8910は全ch共有)
//      ch0はBank A(reg 0xb/0xc)、ch1/ch2はBank B(reg 0x0-0x3)に配置される
//      非対称な構造 (旧FITOM完全準拠)
//    - デューティ比制御 (reg 0x6-0x8、Bank B。無印AY-3-8910にはない機能)
//
//  hwOp[0].WS (4bit) をデューティ比として転用する (SCCの波形番号とは
//  無関係、チップ依存の意味の転用パターン)。
// ================================================================
class CEPSG : public CPSGBase {
public:
    // 内部シャドウ上のBank Bオフセット。実チップのアドレスは4bit(0x0-0xF)
    // のままで、Bank A/Bはレジスタ0x0Dのbit4で切り替える多重化方式のため、
    // アドレスだけではバンクを区別できない。regBak_(と後述のレジスタビュー)
    // ではBank Bを0x10以降へ分離して「32レジスタ」として持ち、チップへ送る
    // 際に元の4bitアドレスへ戻す。この番号体系はAY8930のエミュレータ実装が
    // 一般に採る表現(MAMEのay8910_deviceも register_latch + (mode&1)<<4 と
    // している)と一致する。
    static constexpr uint16_t kBankB = 0x10;

    CEPSG(IPort* port, int sampleRate)
        : CPSGBase(DEVICE_EPSG, port, kBankB * 2, 3, sampleRate)
    {
        opCount_ = 2;
    }

    std::string getDescriptor() const override { return "AY8930 (EPSG) 3ch"; }

    void init() override {
        // Bank B
        writeBank(1, 0x09, 0xff, true); // Noise AND mask
        writeBank(1, 0x0a, 0x00, true); // Noise OR mask
        writeBank(1, 0x04, 0x00, true); // ch1 エンベロープシェイプ
        writeBank(1, 0x05, 0x00, true); // ch2 エンベロープシェイプ
        // Bank A
        writeBank(0, 0x07, 0x3f, true); // 全チャンネル無効化
        writeBank(0, 0x08, 0x00, true);
        writeBank(0, 0x09, 0x00, true);
        writeBank(0, 0x0a, 0x00, true);
    }

    // バンク切り替えを持つため、ポートのシャドウレジスタでは表現できない。
    // ドライバ自身が持つバンク付きシャドウをレジスタダンプへ提供する。
    bool getBankedRegisterView(std::vector<uint8_t>& out) const override {
        if (!regBak_) return false;
        out.assign(regBak_, regBak_ + regSize_);
        return true;
    }

protected:
    uint8_t prevMix_ = 0x3f;
    uint8_t curBank_ = 0xFF;   // 未確定 (最初のwriteBankで必ず選択させる)

    // レジスタ0x0D: bit7-5=101で拡張モード有効、bit4=バンク選択、
    // bit3-0=ch0のエンベロープシェイプ。シェイプは保持したまま切り替える。
    void selectBank(uint8_t bank) {
        uint8_t shape = static_cast<uint8_t>(getReg(0x0d) & 0x0f);
        uint8_t v = static_cast<uint8_t>((bank ? 0xb0 : 0xa0) | shape);
        if (curBank_ == bank && getReg(0x0d) == v) return;
        curBank_ = bank;
        // 0x0D自体は両バンク共通のレジスタなのでバンク付きにしない
        setReg(0x0d, v, true);
    }

    // バンク付きレジスタ書き込み。シャドウは bank<<8 | reg でキャッシュし、
    // チップへは8bitアドレスで送る。同値スキップ・read-modify-writeが
    // バンクごとに正しく効くようになる。
    void writeBank(uint8_t bank, uint8_t reg, uint8_t data, bool force = false) {
        uint16_t shadow = static_cast<uint16_t>(reg | (bank ? kBankB : 0));
        if (!force && getReg(shadow) == data) return;
        selectBank(bank);
        if (regBak_ && shadow < regSize_) regBak_[shadow] = data;
        if (port_) port_->write(reg, data);
    }

    uint8_t readBank(uint8_t bank, uint8_t reg) const {
        return getReg(static_cast<uint16_t>(reg | (bank ? kBankB : 0)));
    }

    // ch0のエンベロープシェイプは0x0Dの下位4bitに同居するため、
    // バンク選択と同時に書く必要がある。
    void writeEnvShapeCh0(uint8_t shape) {
        setReg(0x0d, static_cast<uint8_t>((curBank_ ? 0xb0 : 0xa0) | (shape & 0x0f)), true);
    }

    // AY8930のExpand Modeはトーン周期が16bitに拡張され、分周比も
    // 無印AY-3-8910の1/16から1/8へ変わる。同じ音程に対する周期値は
    // SSGテーブル(1/16基準)の2倍になるため、chipShift=-1を与える。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint16_t etp = tonePeriod(fnum, 0xFFFF, -1);
        writeBank(0, static_cast<uint8_t>(ch * 2 + 0), static_cast<uint8_t>(etp & 0xFF));
        writeBank(0, static_cast<uint8_t>(ch * 2 + 1), static_cast<uint8_t>(etp >> 8));
    }

    // HWエンベロープ未使用時のみ5bit音量を計算 (旧FITOM完全準拠、
    // Linear2dBの結果を31から引く反転極性)。
    // linear2dBの最終シフト(7-range-bw)で既にレジスタ幅相当のdBステップに
    // 変換されるため、evol側はSTEP075DB(無マスク)で0-127をフルレンジの
    // まま渡す必要がある。STEP150DBでマスクするとevolの上位bitが失われ、
    // 64以上の値が0-63へ折り返されて音量が不連続になるバグになる
    // (OPLLで発見された同種の不具合と同一原因)。
    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        if (s.hwPatch.hwOp[0].EGT & 0x08) return; // HW EG使用時はチップが自動制御
        uint8_t loudness = computeFinalLoudness(ch);
        uint8_t vol = static_cast<uint8_t>(
            31 - fitom::linear2dB(loudness, RANGE48DB, STEP075DB, 5));
        writeBank(0, static_cast<uint8_t>(0x08 + ch), static_cast<uint8_t>(vol & 0x1F));
    }

    // トーン/ノイズ有効ビット・HWエンベロープパラメータ・デューティ比を設定。
    // バンクの選択はwriteBank()が必要に応じて行うため、ここで明示的に
    // 戻す必要はない。
    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        resetLfoBaseline(ch);

        // ミックス値計算 (CSSGのALG意味論と同じ: 0=トーン/1=ノイズ/2=両方/3=両方)
        uint8_t mix = 0x8;
        switch (p.hw.ALG & 3) {
        case 0: mix = 0x8; break;
        case 1: mix = 0x1; break;
        case 2: mix = 0x0; break;
        case 3: mix = 0x9; break;
        }
        prevMix_ = static_cast<uint8_t>((prevMix_ & ~(0x9u << ch)) | (mix << ch));
        writeBank(0, 0x07, prevMix_, true);
        if (mix == 1 || mix == 0) {
            writeBank(0, 0x06,
                      static_cast<uint8_t>((p.hw.NFQ & 0x1F) | ((p.hw.FB & 7) << 5)), true);
        }

        bool hwEnv23 = false;
        if (p.hwOp[0].EGT & 0x08) { // HWエンベロープ使用
            writeBank(0, static_cast<uint8_t>(0x08 + ch), 0x20, true);
            if (ch == 0) {
                // ch0はBank A内のreg 0xb/0xcと、0x0D下位4bitのシェイプを使う
                writeBank(0, 0x0b,
                          static_cast<uint8_t>(((p.hwOp[0].SL << 4) & 0xF0) | (p.hwOp[0].RR & 0xF)), true);
                writeBank(0, 0x0c,
                          static_cast<uint8_t>(((p.hwOp[0].DR << 4) & 0xF0) | (p.hwOp[0].SR & 0xF)), true);
                writeEnvShapeCh0(p.hwOp[0].EGT & 0xF);
            } else {
                hwEnv23 = true; // ch1/ch2はBank B側で設定する
            }
        }

        // デューティ比 (Bank B、hwOp[0].WSを流用)
        writeBank(1, static_cast<uint8_t>(0x06 + ch),
                  static_cast<uint8_t>(p.hwOp[0].WS & 0xF), true);
        if (hwEnv23) {
            writeBank(1, static_cast<uint8_t>(0x00 + (ch - 1) * 2),
                      static_cast<uint8_t>(((p.hwOp[0].SL << 4) & 0xF0) | (p.hwOp[0].RR & 0xF)), true);
            writeBank(1, static_cast<uint8_t>(0x01 + (ch - 1) * 2),
                      static_cast<uint8_t>(((p.hwOp[0].DR << 4) & 0xF0) | (p.hwOp[0].SR & 0xF)), true);
            writeBank(1, static_cast<uint8_t>(0x03 + ch),
                      static_cast<uint8_t>(p.hwOp[0].EGT & 0xF), true);
        }

        updateVolExp(ch);
    }

    // HWエンベロープ使用時: キーオンでupdateVoice再実行 (エンベロープを
    // リスタートさせる)、キーオフで音量レジスタを0クリア。
    // 非使用時は基底CPSGBase::updateKey (ソフトウェアエンベロープ) に委譲。
    void updateKey(uint8_t ch, bool keyOn) override {
        const auto& s = chState_[ch];
        if (s.hwPatch.hwOp[0].EGT & 0x08) {
            if (keyOn) {
                updateVoice(ch);
            } else {
                writeBank(0, static_cast<uint8_t>(0x08 + ch),
                          static_cast<uint8_t>(readBank(0, static_cast<uint8_t>(0x08 + ch)) & 0xC0),
                          true);
            }
        } else {
            CPSGBase::updateKey(ch, keyOn);
        }
    }

    // op=0: 振幅LFO (基底のupdateTLに委譲)
    // op=1: ノイズ周波数LFO (ミックスがノイズ関連の場合のみ、旧FITOM完全準拠)
    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        if (op == 0) {
            CPSGBase::updateTL(ch, op, lev);
            return;
        }
        if (op == 1) {
            const auto& p = chState_[ch].hwPatch;
            if ((p.hw.ALG & 3) == 1 || (p.hw.ALG & 3) == 2) {
                int16_t frq = static_cast<int16_t>(lev) - 64
                             + static_cast<int16_t>(((p.hw.NFQ << 2) | (p.hw.FB >> 1)) & 0x7F);
                frq = std::clamp<int16_t>(frq, 0, 255);
                writeBank(0, 0x06, static_cast<uint8_t>(frq), true);
            }
        }
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
        // M_PI_2 は POSIX/GNU拡張でありMSVC標準では未定義のため、
        // 移植性のためリテラル値 (π/2) を直接使う。
        constexpr double kHalfPi = 1.5707963267948966;
        double lgain = std::cos(kHalfPi * p / 126.0);
        double rgain = std::sin(kHalfPi * p / 126.0);
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
std::unique_ptr<ISoundDevice> createCSSGS(IPort* p, int sr, uint32_t deviceType) {
    if (deviceType == DEVICE_SSGS3) return std::make_unique<CSSGS3>(p, sr);
    return std::make_unique<CSSGS>(p, sr, static_cast<uint8_t>(deviceType));
}
std::unique_ptr<ISoundDevice> createCEPSG(IPort* p, int sr) { return std::make_unique<CEPSG>(p, sr); }

// ================================================================
//  フォールバック受け入れ判定
// ================================================================
// CSSG (VOICE_PATCH_SSG): AY8930形式の音色データを再生できるか。
// AY8930拡張のデューティ比 (hwOp[0].WS) が未使用(デフォルト=0)の
// 場合のみ安全 (無印SSGはデューティ比制御ハードウェアを持たないため、
// 非デフォルト値を無視すると波形が変わってしまう)。
bool cssgAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch) {
    if (sourceVoicePatchType != VOICE_PATCH_EPSG) return false;
    return patch.hwOp[0].WS == 0;
}

// CEPSG (VOICE_PATCH_EPSG): SSG形式の音色データは常に安全に再生できる
// (SSG音色はデューティ比フィールド自体を使わない=0のままのため)。
bool cepsgAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& /*patch*/) {
    return sourceVoicePatchType == VOICE_PATCH_SSG;
}

std::unique_ptr<ISoundDevice> createCDCSG(IPort* p, int sr) { return std::make_unique<CDCSG>(p, sr); }
std::unique_ptr<ISoundDevice> createCSCC(IPort* p, int sr, uint32_t deviceType) {
    return std::make_unique<CSCC>(p, sr, static_cast<uint8_t>(deviceType));
}
std::unique_ptr<ISoundDevice> createCSAA1099(IPort* p, int sr) { return std::make_unique<CSAA1099>(p, sr); }
} // namespace fitom
