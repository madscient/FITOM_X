// fitom/DSG_new.cpp
// CDSG / CDSGRhythm チップドライバ (YM2163 DSG: Digital Sound Generator)
//
// YM2163 の特徴:
//   - 波形メモリ方式。楽音4ch + 内蔵リズム(BD/HC/SDN/HHO/HHD)
//   - 音色はROM固定 (エンベロープ4種 × 波形5種)。ユーザー音色は持たない
//   - チップ内蔵EG。PSG系のようなソフトウェアADSRは使わない
//   - サスティンON/OFFをハードウェアで持つ (reg 0x88+ch の SUS ビット)
//   - 音量は2bit (0/-6/-12/-∞ dB) しか無い
//   - 実チップは単一のバスアドレスに対し「D7=1でアドレスラッチ / D7=0で
//     データ」の2回書き込みで駆動する。この2回書き込みはHW I/Fプラグイン
//     (DSGemuEngine等) 側が吸収するため、ドライバからは通常の
//     setReg(addr, data) で 0x80-0x9F のレジスタを直接叩く。
//     ポートは1本のみでSplitPort/extraPortは使わない。
//
// レジスタマップ (データシート表2「DSGアドレスマップ」):
//   0x80-0x83  DV4 DV3 DV2 DV1 DV0 DV1/2 DV1/4   分周数(下位7bit)
//   0x84-0x87  KON FD B2 B1 DV7 DV6 DV5          キイオン/強制減衰/オクターブ/分周数(上位)
//   0x88-0x8B  E2 E1 SUS - W3 W2 W1              エンベロープ/サスティン/波形メモリ
//   0x8C-0x8F  TEST VL2 VL1 F4 F3 F2 F1          音量/出力端子選択
//   0x90-0x93  FGR IEN HHD HHO SDN HC BD         リズムトリガー (4アドレスはミラー)
//   0x94-0x97  - LV4 LV3 LV2 LV1 LV0 LH          リズムレベル (HH/BD/HC/SDN)
//   0x98-0x9F  タイマー周期 (FITOMでは未使用)

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/FnumUtils.h"
#include "fitom/VolumeUtils.h"
#include "fitom/Log.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace fitom {

// レジスタ空間は 0x80-0x9F だが、regBak_ はレジスタアドレスをそのまま
// 添字にする実装 (CSoundDevice::setReg) のため 0xA0 バイト確保する。
constexpr int kDsgRegSize = 0xA0;

// ================================================================
//  CDSG — YM2163 楽音部 (4ch)
// ================================================================
//
//  PSG系 (CPSGBase) は継承しない。あちらはチップ内蔵EGが貧弱なため
//  ソフトウェアADSRで音量を時間変化させる設計だが、YM2163は4種類の
//  内蔵エンベロープを持ち音色パラメータ自体を受け付けないため、
//  ソフトウェアEGを走らせる余地が無い。
//
//  周波数も共有の周期テーブル (FnumTableType::TonePeriod) は使わない。
//  あのテーブルは SN76489/SCC の f = master/(32*N) 用に量子化されており、
//  総分周比が32倍細かいYM2163 (f = clock/(DV * 2^(3-oct))) に流用すると
//  基準オクターブでの丸め (テーブル値が119-239程度しかない) がそのまま
//  乗算されて最悪14セント近い音痴になる。CSAA1099 が実機式を直接
//  計算しているのと同じ理由で、getFnumber() でHzから直接分解する。
// ================================================================
class CDSG : public CSoundDevice {
public:
    CDSG(IPort* port, int masterClock)
        : CSoundDevice(DEVICE_DSG, 4, port,
                       masterClock, 1,
                       FNUM_OFFSET, FnumTableType::TonePeriod, kDsgRegSize)
    {
        opCount_ = 1;
        lfoTL_.assign(maxChs_, 64);
    }

    std::string getDescriptor() const override { return "DSG (YM2163) 4ch"; }

    void init() override {
        for (uint8_t ch = 0; ch < 4; ++ch) {
            setReg(static_cast<uint16_t>(0x84 + ch), 0x00, true); // KON/FD クリア
            setReg(static_cast<uint16_t>(0x8C + ch), 0x30, true); // 音量-∞ かつ全端子OFF
        }
    }

protected:
    std::vector<uint8_t> lfoTL_;   // ソフトLFO の基準TL (振幅変調用)

    // 音色パラメータの持ち方 (PatchManager::initDsgBuiltinPatches と対):
    //   hw.ALG      = エンベロープ番号 E2E1 (0-3)
    //   hwOp[0].WS  = 波形メモリ番号 W3W2W1 (1-5、0/6/7は未定義=無音)
    static uint8_t envOf(const HwPatch& p)  { return static_cast<uint8_t>(p.hw.ALG & 3); }
    static uint8_t waveOf(const HwPatch& p) { return static_cast<uint8_t>(p.hwOp[0].WS & 7); }

    // 出力端子選択 (reg 0x8C+ch の F1-F4)。OR1-OR4 は実機では外部の負荷抵抗で
    // ミックスする独立した端子で、パンポットではない (どれを選んでも定位は
    // 変わらない)。複数選ぶと同じ信号が複数端子へ出て加算されるだけなので、
    // 常に OR1 のみを使う。
    static constexpr uint8_t kOutputPins = 0x01;

    // 1オペレータ相当の波形メモリ音源であり、op0が唯一の発音オペレータ。
    // 基底のデフォルト実装は hw.ALG を OPN/OPM の8アルゴリズム番号として
    // 引くが、本チップの hw.ALG はエンベロープ番号に転用しているため
    // (OPLLがプリセット音色番号に転用しているのと同じ事情) 必ず上書きする。
    bool isCarrierOp(uint8_t /*ch*/, int op) const override { return op == 0; }

    // vol × exp × vel × 音色TL × 振幅LFO を統合したラウドネス (0-127、高いほど大)。
    // 時間変化はチップ内蔵EGが受け持つため、PSG系の
    // CPSGBase::computeFinalLoudness() と違いソフトウェアEG成分は持たない。
    uint8_t computeDsgLoudness(uint8_t ch) const {
        const auto& s = chState_[ch];
        uint8_t base   = fitom::calcVolExpVel(s.volume, s.expression, s.velocity);
        uint8_t withTL = fitom::calcLinearLevel(base, s.hwPatch.hwOp[0].TL);
        int atten = std::clamp(static_cast<int>(lfoTL_[ch]) - 64, 0, 127);
        return fitom::calcLinearLevel(withTL, static_cast<uint8_t>(atten));
    }

    void updateVoice(uint8_t ch) override {
        lfoTL_[ch] = 64;   // 振幅LFOの基準値へ戻す
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        // エンベロープ / サスティン / 波形。SUS は現在のサスティンペダル状態を
        // そのまま反映する (updateSustain と同じ式)。
        setReg(static_cast<uint16_t>(0x88 + ch),
               static_cast<uint8_t>((envOf(p) << 5) | (s.sustain ? 0x10u : 0u) | waveOf(p)));
        updateVolExp(ch);
    }

    // 音量レジスタ VL2VL1 は 0dB / -6dB / -12dB / -∞ の4段階しか無い。
    // linear2dB() は 0.75dB の2のべき乗倍のステップしか刻めず 6dB を表現
    // できない (2bit幅では12dB/stepになる) ため、CDCSG が 2dB/step のために
    // kGM2dB から直接換算しているのと同じ方式をとる。
    void updateVolExp(uint8_t ch) override {
        int steps = static_cast<int>(std::lround(-fitom::kGM2dB[computeDsgLoudness(ch)] / 6.0));
        uint8_t vl = static_cast<uint8_t>(std::clamp(steps, 0, 3));
        setReg(static_cast<uint16_t>(0x8C + ch),
               static_cast<uint8_t>((vl << 4) | kOutputPins), false);
    }

    void updateTL(uint8_t ch, uint8_t op, uint8_t lev) override {
        if (op == 0) { // 振幅LFO
            lfoTL_[ch] = lev;
            updateVolExp(ch);
        }
    }

    // 発音周波数 f = clock / (DV * 2^(3-oct))、oct = B2*2+B1 (0-3)。
    // DV は DV7〜DV1/4 を並べた10bit整数 (LSB=DV1/4) で、データシートの
    // 分周数 ΣDVi の4倍値にあたる。
    //
    // ChState::Fnum へは block=oct / fnum=DV として詰める (GUIのFnumber表示も
    // 実機レジスタと同じ値になる)。共有の周期テーブルを使わない理由は
    // クラス先頭のコメントを参照。
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        ChState::Fnum ret;
        if (ch >= maxChs_) return ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;

        // kfs単位 (1半音=64ステップ)。チャンネルLFOの寄与も
        // CSoundDevice::getFnumber() と同じ扱いで加える。
        const int16_t lfoOffset = s.proc.channelLfoActive() ? s.proc.channelLfoValue() : 0;
        const double kfs = static_cast<double>(s.lastNote) * 64.0
                         + static_cast<double>(s.fineFreq) + offset + lfoOffset;
        const double semitones = kfs / 64.0 - 69.0;   // A4 (MIDIノート69) 基準
        const double hz = FnumRegistry::instance().getMasterPitch()
                        * std::pow(2.0, semitones / 12.0);
        if (hz <= 0.0) return ret;

        // 総分周比 clock/f を DV * 2^(3-oct) へ分解する。octが大きいほど
        // 2^(3-oct) が小さくなりDVの有効桁が増えるため、DVが10bitに収まる
        // 範囲で最小のシフト量 (=最大のoct) を選ぶ。
        const double totalDiv = static_cast<double>(fnumMaster_) / hz;
        int shift = 0;
        while (shift < 3 && totalDiv / (1 << shift) > 1023.0) ++shift;
        const long dv = std::lround(totalDiv / (1 << shift));

        ret.fnum  = static_cast<uint16_t>(std::clamp<long>(dv, 1, 1023));
        ret.block = static_cast<uint8_t>(3 - shift);
        return ret;
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        const uint16_t dv = static_cast<uint16_t>(fnum.fnum & 0x3FF);
        const uint8_t oct = static_cast<uint8_t>(fnum.block & 3);

        setReg(static_cast<uint16_t>(0x80 + ch), static_cast<uint8_t>(dv & 0x7F), false);
        // KON は同じレジスタの bit6 にあるため必ず現在値を保存する。
        // 実機/エミュレータともKONはエッジ検出のため、発音中にここを
        // 書き替えてもリトリガーはされない (ピッチベンドで必要)。
        uint8_t kon = getReg(static_cast<uint16_t>(0x84 + ch)) & 0x40;
        const uint8_t b2 = static_cast<uint8_t>(((oct >> 1) & 1) << 4);
        const uint8_t b1 = static_cast<uint8_t>((oct & 1) << 3);
        setReg(static_cast<uint16_t>(0x84 + ch),
               static_cast<uint8_t>(kon | b2 | b1 | ((dv >> 7) & 0x07)), false);
    }

    void updatePanpot(uint8_t /*ch*/) override {
        // YM2163 はパンポット非対応 (OR1-OR4 は定位ではなく出力端子の選択)。
    }

    // サスティンペダル → SUS ビット (reg 0x88+ch bit4)。
    // SUS=1 でキーオフ後のリリースが 1.2秒へ延び、エンベロープ0では
    // キーオフ自体が効かなくなる (データシート図2)。ROM固定音色で
    // EGパラメータを持たないため、OPLL と同じくSUSビットだけで制御する。
    void updateSustain(uint8_t ch) override {
        const uint8_t sus = chState_[ch].sustain ? 0x10u : 0x00u;
        const uint8_t cur = getReg(static_cast<uint16_t>(0x88 + ch));
        setReg(static_cast<uint16_t>(0x88 + ch),
               static_cast<uint8_t>((cur & 0xEFu) | sus));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        // KON(bit6)のみを操作し、FD(bit5)は落とす。同じレジスタに同居する
        // 分周数上位/オクターブ(bit4-0)は保存する。
        uint8_t cur = getReg(static_cast<uint16_t>(0x84 + ch)) & 0x1Fu;
        setReg(static_cast<uint16_t>(0x84 + ch),
               static_cast<uint8_t>(cur | (keyOn ? 0x40u : 0u)), true);
    }

    // CC#120 (All Sound Off): SUS を落としてから FD (フォーシングダンプ) を
    // 立て、通常のリリースより速く消音する。FD もKONと同じくエッジ検出
    // なので、後続の noteOff() が KON/FD をまとめて0に戻すことで次回の
    // 立ち上がりが再武装される。
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const uint8_t susCur = getReg(static_cast<uint16_t>(0x88 + ch));
        setReg(static_cast<uint16_t>(0x88 + ch), static_cast<uint8_t>(susCur & 0xEFu));
        const uint8_t cur = getReg(static_cast<uint16_t>(0x84 + ch));
        setReg(static_cast<uint16_t>(0x84 + ch), static_cast<uint8_t>(cur | 0x20u), true);
        noteOff(ch);
    }
};

// ================================================================
//  CDSGRhythm — YM2163 内蔵リズム音源 (5パート)
//
//  パート番号はトリガービット (reg 0x90 の D0-D4) の並びに一致させる:
//    0=BD (バスドラム)      → RH1、レベル reg 0x95
//    1=HC (ハイコンガ)      → RH1、レベル reg 0x96
//    2=SDN (スネアノイズ)   → RH2、レベル reg 0x97
//    3=HHO (ハイハットOpen) → RH2、レベル reg 0x94
//    4=HHD (ハイハットClose)→ RH2、レベル reg 0x94
//  HHO/HHD は実機のリズム音源としては同一の発振器を共有するため、
//  レベルレジスタも 0x94 を共有する (OPLLリズムのHH/SDがch7を共有するのと
//  同じ構図)。
//
//  楽音部 (CDSG) とはレジスタ空間が完全に独立しているため、OPL/OPLL系の
//  ように rhythm_mode でFM側chを無効化する必要が無く、OPNAの
//  DEVICE_OPNA_RHY と同様に常時生成してよい
//  (Config::resolveCompositeSpec 参照)。
// ================================================================
class CDSGRhythm : public CSoundDevice {
public:
    // リズム音には音程レジスタが無いためFnumテーブルは使わないが、
    // 基底クラスがテーブルを構築できるよう楽音部と同じ引数を渡しておく。
    CDSGRhythm(IPort* port, int masterClock)
        : CSoundDevice(DEVICE_DSG_RHY, 5, port,
                       masterClock, 1,
                       FNUM_OFFSET, FnumTableType::TonePeriod, kDsgRegSize)
    {}

    std::string getDescriptor() const override { return "DSG Rhythm 5ch"; }

    void init() override {
        setReg(0x90, 0x00, true);                     // トリガー/IRQ全クリア
        for (uint16_t r = 0x94; r <= 0x97; ++r)
            setReg(r, 0x00, true);                    // LH=0 / レベル最大
    }

protected:
    // パート → トリガービット / レベルレジスタ
    static constexpr uint8_t  kTriggerBit[5] = {0x01, 0x02, 0x04, 0x08, 0x10};
    static constexpr uint16_t kLevelReg[5]   = {0x95, 0x96, 0x97, 0x94, 0x94};

    void updateVoice(uint8_t ch) override { updateVolExp(ch); }

    void updateFreq(uint8_t /*ch*/, const ChState::Fnum* /*fn*/) override {
        // リズム音の音程を変えるレジスタは存在しない。
    }
    void updateSustain(uint8_t /*ch*/) override {}
    void updatePanpot(uint8_t /*ch*/) override {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

    // リズムレベル LV4-LV0 は「0が最大音量、31が最小音量」の線形減衰
    // (出力振幅 ∝ (32-LV)/32、31でも無音にはならない)。SCC/SAA と同じく
    // リニアレジスタなので linear2dB() は通さず線形にスケールする。
    //
    // LH=0 のまま書くことで、設定したレベルを起点に内蔵リズムEGの減衰が
    // 進む (LH=1 は減衰せず固定レベルで鳴り続けるモード)。ただしトリガーは
    // レベルを最大へリセットするため、ベロシティを効かせるには
    // 「トリガー → レベル書き込み」の順でなければならない (updateKey参照)。
    void writeLevel(uint8_t ch) {
        const auto& s = chState_[ch];
        // Expression を含めない点は COPLLRhythm::updateVolExp と揃える。
        uint8_t loudness = fitom::calcVolExpVel(s.volume, 127u, s.velocity);
        int lv = 32 - (static_cast<int>(loudness) * 32 + 63) / 127;
        lv = std::clamp(lv, 0, 31);
        setReg(kLevelReg[ch], static_cast<uint8_t>((lv & 0x1F) << 1), true);
    }

    void updateVolExp(uint8_t ch) override { writeLevel(ch); }

    void updateKey(uint8_t ch, bool keyOn) override {
        if (ch >= maxChs_) return;
        // NoteOff では何も書かない。リズムトリガーはワンショットで、
        // キーオフに相当する機構自体が無い (COPNARhythm と同じ判断)。
        if (!keyOn) return;
        // reg 0x90 は書いたビットが自動的に0へ戻るトリガーレジスタなので、
        // 他パートのビットをORしてはならず、また同じ値の連打を
        // シャドウキャッシュに抑止されないよう forceWrite で書く。
        setReg(0x90, kTriggerBit[ch], true);
        // トリガーが内蔵EGのレベルを最大へリセットした後にレベルを書く
        // ことで、ベロシティを起点として減衰させる (writeLevel 参照)。
        writeLevel(ch);
    }

    // パート番号は音色データの hw.ALG (下位3bit) で直接指定する
    // (COPLLRhythm::queryCh と同じ規約)。リズムトリガーは自動的に0へ戻り
    // 「使用中」を読み出す手段が無いため、使用中判定は行わない。
    uint8_t queryCh(IMidiCh* /*owner*/, const HwPatch* patch, int /*mode*/) override {
        if (!patch) return 0xFF;
        uint8_t num = static_cast<uint8_t>(patch->hw.ALG & 0x7);
        return (num < maxChs_) ? num : 0xFF;
    }
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCDSG(IPort* p, int sr) {
    return std::make_unique<CDSG>(p, sr);
}
std::unique_ptr<ISoundDevice> createCDSGRhythm(IPort* p, int sr) {
    return std::make_unique<CDSGRhythm>(p, sr);
}
} // namespace fitom
