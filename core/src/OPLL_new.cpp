// fitom/OPLL_new.cpp
// COPLL / COPLL2 / COPLLP チップドライバ — ISoundDevice ベース移行版
//
// OPLL の特徴:
//   - 9ch / 2OP
//   - ユーザー音色 (reg 0x00-0x07) またはプリセット音色 (reg 0x30 bit7-4)
//   - ALG & 0x40 でプリセット選択フラグ (HwPatch では ext.ALG_EXT として管理)
//   - ボリュームはレジスタ 0x30 の下位 4bit (4bit = 16段階)
//   - リズムモード: ch 6-8 はリズムとして使用 (有効/無効を enableCh で管理)

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/VolumeUtils.h"
#include "fitom/Log.h"
#include <algorithm>

namespace fitom {

// OPLL系(YM2413/YM2420/YMF281/YM2423/FS1001=VRC7)の実機マスタークロック。
// OPL系と同じ3.579545MHz(NTSC colorburst由来)を使う共通ファミリーのため、
// OPL_new.cppのkMasterClockと同値・同じdivide=72。
// sampleRate引数は他チップドライバとのファクトリ関数シグネチャ一貫性のために
// 残すが、Fnum計算には使用しない(OPL_new.cppで2026年7月に発見・修正した
// 「誤ってsampleRateをfnumMasterとして使っていたため、Fnumberが常に65535に
// クランプされ意図した音程にならない」バグと同種のものがOPLLにも残っていた)。
constexpr int kOpllMasterClock = 3579545;

class COPLL : public CSoundDevice {
public:
    // mode: 0=トーンのみ (9ch), 1=リズムモード (6ch + リズム)
    // maxChs: 物理チャンネル数。VRC7 (リズム回路なし) は 6 を渡す。
    //         chState_はmaxChs分しか確保されないため、ch6-8の無効化は
    //         maxChs自体で打ち切る(VRC7のmaxChs=6は現状常にmode=0で
    //         呼ばれるため実際には到達しないが、範囲外アクセス防止の
    //         防御的チェックとして残す)。
    COPLL(IPort* port, int /*sampleRate*/, uint8_t mode = 0,
          uint8_t devId = DEVICE_OPLL, uint8_t maxChs = 9)
        : CSoundDevice(devId, maxChs, port,
                       kOpllMasterClock, 72,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x40)
        , rhythmMode_(mode)
    {
        opCount_ = 2;
        if (rhythmMode_) {
            // ch 6-8 をリズム専用として自動割り当て禁止
            for (int i = 6; i < 9 && i < maxChs_; ++i) chState_[i].disable();
        }
    }

    // getDescriptor: リズムモードの有無を反映する。
    // "OPLL (YM2413) 9ch" または "OPLL (YM2413) 6ch + Rhythm 5ch"
    // 派生クラスは chipLabel() だけをオーバーライドすればよい。
    std::string getDescriptor() const override {
        std::string label = chipLabel();
        if (rhythmMode_) {
            return label + " " + std::to_string(maxChs_ - 3) + "ch + Rhythm 5ch";
        }
        return label + " " + std::to_string(maxChs_) + "ch";
    }
    void init() override {}

    void reset() override {
        CSoundDevice::reset();
        if (rhythmMode_) {
            for (int i = 6; i < 9 && i < maxChs_; ++i) chState_[i].disable();
        }
    }

protected:
    bool rhythmMode_;

    // 派生クラスがチップ名部分だけを差し替えるためのフック。
    virtual std::string chipLabel() const { return "OPLL (YM2413)"; }

    // VoiceProcessor へ渡すキャリアマスク計算用 (2026年7月追加)。
    // OPLLはキャリアが常にop1固定(car_opll = (i==1)、updateVoice参照)。
    // hw.ALGはOPLLではプリセット音色番号に転用されており、OPN/OPM用の
    // デフォルト実装(hw.ALGを3bit8アルゴリズムとして解釈)とは無関係の
    // ため、オーバーライドする。
    bool isCarrierOp(uint8_t /*ch*/, int op) const override {
        return op == 1;
    }

    // OPLL 専用: プリセットか否かで UpdateVoice 挙動が変わる
    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;

        // ext.ALG_EXT: bit0 = プリセット選択フラグ (旧 AL & 0x40)
        bool preset = (p.ext.ALG_EXT & 1) != 0;
        uint8_t instNo = preset ? (p.hw.ALG & 0xF) : 0;

        if (!preset) {
            // ユーザー音色レジスタへ書き込み (0x00-0x07)
            for (int i = 0; i < 2; ++i) {
                const FmHwOp& o = p.hwOp[i];
                const bool car_opll = (i == 1); // OPLLはOP1がキャリア固定
                // SR (キャリアはベロシティ補正後の値)。EGTビットとRRレジスタの
                // 両方がこの値で決まるため、先に求めておく。
                const uint8_t sr_opll = car_opll ? s.proc.velSR(i) : (o.SR & 0x1F);
                // AM/VIB/EG/KSR/MUL
                // EGT: SR>0 (キーオン中もRRレジスタのレートで減衰させたい)
                // なら decay(0)、それ以外は sustained(1)。
                // OPLLはこの静的変換のみで完結させ、キーオン/キーオフ時の
                // 動的な書き換えは行わない (updateKey()参照)。
                setReg(static_cast<uint16_t>(i),
                       static_cast<uint8_t>(
                           ((o.AM & 1) << 7) | ((o.VIB & 1) << 6) |
                           ((sr_opll > 0) ? 0 : 0x20) |
                           ((o.KSR & 1) << 4) | (o.MUL & 0xF)));
                // AR / DR (キャリア=i:1 はベロシティ補正)
                const uint8_t ar_opll = car_opll ? s.proc.velAR(i) : (o.AR & 0x1F);
                const uint8_t dr_opll = car_opll ? s.proc.velDR(i) : (o.DR & 0x1F);
                setReg(static_cast<uint16_t>(4 + i),
                       static_cast<uint8_t>(((ar_opll >> 1) << 4) | (dr_opll >> 1)));
                // SL / RR
                // RRレジスタは上のEGTビットと対で意味が決まるため、同じ静的
                // 変換規則で値を選ぶ:
                //   SR>0 (EGT=0/decay)     → SRを4bit変換した値 (キーオン中の
                //                            減衰レイト。キーオフ後のリリースも
                //                            同じレートになる)
                //   SR==0 (EGT=1/sustain)  → RRをそのまま (キーオフ後のリリース)
                // (SR>0の音色はレジスタイメージ由来ではRRフィールドが未使用=0で
                //  あることが多く、ここでRRを書いてしまうと減衰しなくなる。
                //  voice-parameter-reference.mdのOPLL節の変換表を参照)
                // Sustain (サステインペダル) は SUS bit で制御するため、
                // ここでRRを細工する必要はない。
                const uint8_t sl_opll = car_opll ? s.proc.velSL(i) : (o.SL & 0xF);
                const uint8_t rr_opll = (sr_opll > 0)
                                      ? static_cast<uint8_t>((sr_opll >> 1) & 0xF)
                                      : (car_opll ? s.proc.velRR(i) : (o.RR & 0xF));
                setReg(static_cast<uint16_t>(6 + i),
                       static_cast<uint8_t>(((sl_opll & 0xF) << 4) | (rr_opll & 0xF)));
            }
            // TL / KSL (op0)
            setReg(0x02, static_cast<uint8_t>(((p.hwOp[0].KSL & 3) << 6) | (p.hwOp[0].TL >> 1)));
            // KSL / WS (op0/op1) / FB
            setReg(0x03, static_cast<uint8_t>(
                ((p.hwOp[1].KSL & 3) << 6) |
                ((p.hwOp[0].WS & 1) << 3) |
                ((p.hwOp[1].WS & 1) << 4) |
                (p.hw.FB & 7)));
        }

        // inst / vol (inst: bit7-4, vol: bit3-0)
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0x0F;
        setReg(static_cast<uint16_t>(0x30 + ch),
               static_cast<uint8_t>((instNo << 4) | cur));
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        // OPLL はキャリア (op1) の effectiveTL (ラウドネス空間、0=無音,127=最大音量。
        // OPN_new.cppのeffTLToReg()コメント参照) をそのままdB変換
        // (48dB/3dBステップ、4bit) する。ステップ幅はlinear2dB内の最終シフト
        // (7-range-bw) で決まるため、evol側はSTEP075DB (無マスク) で
        // 0-127をフルレンジのまま渡す必要がある。STEP150DBでマスクすると
        // evolの上位bitが失われ、64以上の値が0-63へ折り返されて音量が
        // 不連続に無音化するバグになる。
        // (2026年8月、ユーザーの実機確認で「Vol最大で無音、最低で最大音量」
        // という極性反転を発見。effectiveTL()は既にラウドネス空間のため、
        // ここで127-反転を挟むと二重反転になり結果が逆になっていた。
        // ADPCM-A/ADPCM-Bで発見・修正済みの同種バグと同じ誤り)
        uint8_t loudness = s.proc.effectiveTL(1);
        uint8_t vol = fitom::linear2dB(loudness, RANGE48DB, STEP075DB, 4);
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x30 + ch), static_cast<uint8_t>(cur | (vol & 0xF)), false);
    }

    void updateTL(uint8_t ch, uint8_t /*op*/, uint8_t lev) override {
        // OPLL はボリュームレジスタ (0x30 下位 4bit) のみ。lev は呼び出し元
        // (トレモロLFO等) から既にラウドネス空間(0=無音,127=最大音量)で
        // 渡されるため反転不要 (STEP075DBの理由は updateVolExp 参照)
        uint8_t loudness = lev;
        uint8_t vol = fitom::linear2dB(loudness, RANGE48DB, STEP075DB, 4);
        uint8_t cur = getReg(static_cast<uint16_t>(0x30 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x30 + ch), static_cast<uint8_t>(cur | (vol & 0xF)), false);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // getFnumber() は11bit精度で値を返すが、実機OPLLのFnumberは9bit。
        // 旧FITOMの ">>2" (11bit→9bit変換) が新実装で欠落していたため復元する。
        uint16_t fnum9 = static_cast<uint16_t>((fnum.fnum >> 2) & 0x1FF);
        uint8_t b0cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0x30; // KEY/SUSビット保持
        setReg(static_cast<uint16_t>(0x10 + ch),
               static_cast<uint8_t>(fnum9 & 0xFF), false);
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(b0cur | ((fnum.block & 7) << 1) | ((fnum9 >> 8) & 1)), false);
    }

    void updatePanpot(uint8_t /*ch*/) override {
        // OPLL はパンポット非対応
    }

    // CC#120 (All Sound Off): SUS bit を強制的にクリアしてから noteOff。
    // OPLL は RR を直接操作できないため（ROM音色はEG変更不可）、
    // SUS bit を外すことで通常のリリース動作に戻してから noteOff する。
    // これにより sustain 中の音がすぐにリリースフェーズへ移行する。
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const uint8_t cur = getReg(static_cast<uint16_t>(0x20 + ch));
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(cur & 0xDFu)); // SUS bit クリア
        noteOff(ch);
    }

    void updateSustain(uint8_t ch) override {
        // OPLL: 0x20+ch レジスタの bit5 = SUS フラグを操作する。
        // ROM 音色はエンベロープパラメータ変更不可のため、
        // ユーザー音色・ROM 音色を問わず常に SUS bit で制御する。
        // SUS=1: NoteOff 後もチップが無限サスティンレートで引き延ばす。
        // SUS=0: 通常のリリース動作に戻す。
        // 旧 FITOM COPLL::UpdateSustain と同等。
        const auto& s = chState_[ch];
        const uint8_t sus_bit = s.sustain ? 0x20u : 0x00u;
        const uint8_t cur     = getReg(static_cast<uint16_t>(0x20 + ch));
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>((cur & 0xDFu) | sus_bit));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        // OPLLはEGT/RRの動的書き換えを行わず、キーオン/キーオフビットのみを
        // 操作する。
        // EGT/RRはupdateVoice()の静的変換 (SR>0ならEGT=0かつRRレジスタ=SR、
        // SR==0ならEGT=1かつRRレジスタ=RR) で確定済み。
        // OPL系(COPL/COPL3)は従来どおりキーオン/キーオフのたびにEGT/RRを
        // 動的に書き換える技法を使うが、実機OPLLのEG挙動はOPL系と異なり
        // 発音中のEGTビット書き換えが期待どおりに効かないことが2026年8月の
        // 実機検証で確認されたため、OPLL系のみ静的変換とする(確定仕様)
        // (docs/chip-driver-architecture.md 4.4節、
        //  docs/voice-parameter-reference.md OPLL節を参照)。
        uint8_t cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0xEF;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(cur | (keyOn ? 0x10 : 0)), true);
    }
};

// ================================================================
//  COPLL2 / COPLLP / COPLLX — デバイス ID 違いのみ (制御ロジックは共通)
//  内蔵プリセット音色がそれぞれ異なるため、独立したチップとして扱う。
//  リズムモードは OPLL と同じレジスタ構造のためそのまま利用できる。
// ================================================================
class COPLL2 : public COPLL {
public:
    COPLL2(IPort* port, int sampleRate, uint8_t mode = 0)
        : COPLL(port, sampleRate, mode, DEVICE_OPLL2) {}
protected:
    std::string chipLabel() const override { return "OPLL2 (YM2420)"; }

    // YM2420 (OPLL2) は Fnumber のレジスタ配置が YM2413 と異なる。
    // 生の11bit fnum値を直接使用 (COPLL側のような9bit変換は行わない)。
    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        setReg(static_cast<uint16_t>(0x10 + ch),
               static_cast<uint8_t>(((fnum.block & 7) << 5) | ((fnum.fnum >> 6) & 0xFF)), false);
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>((getReg(static_cast<uint16_t>(0x20 + ch)) & 0xF0)
                   | ((fnum.fnum >> 2) & 0xF)), false);
    }
};

class COPLLP : public COPLL {
public:
    COPLLP(IPort* port, int sampleRate, uint8_t mode = 0)
        : COPLL(port, sampleRate, mode, DEVICE_OPLLP) {}
protected:
    std::string chipLabel() const override { return "OPLLP (YMF281B)"; }
};

class COPLLX : public COPLL {
public:
    COPLLX(IPort* port, int sampleRate, uint8_t mode = 0)
        : COPLL(port, sampleRate, mode, DEVICE_OPLLX) {}
protected:
    std::string chipLabel() const override { return "OPLLX (YM2423-X)"; }
};

// ================================================================
//  CVRC7 — OPLL からリズムチャンネルを削除した派生 (FS1001)
//  制御ロジックは OPLL と同一だが、リズム音源回路自体が存在しないため
//  楽音 6ch のみが有効。CSoundDevice に maxChs=6 を渡すことで
//  getChCount() も正しく 6 を返し、chState_自体がch0-5の6要素しか
//  持たなくなるため、ch 6-8 は物理的に存在しない(=常にアクセス不能)
//  扱いになる。リズム回路自体が存在しないため、rhythmMode は常に
//  無効に固定する(呼び出し元から true が渡されても無視する)。
// ================================================================
class CVRC7 : public COPLL {
public:
    CVRC7(IPort* port, int sampleRate)
        : COPLL(port, sampleRate, /*mode=*/0, DEVICE_VRC7, 6) {}
protected:
    std::string chipLabel() const override { return "VRC7 (FS1001)"; }
};

// ================================================================
//  COPLLRhythm: YM2413 (OPLL) 内蔵リズム音源 (5パート: HH/CYM/TOM/SD/BD)
//
//  FM本体とは独立したレジスタ体系:
//    0x0E: bit5=リズムモード有効固定、bit0-4=各パートのキーオン
//    0x36/0x37/0x38: パート音量 (2パートずつ上位/下位4bitに packing)
//    パート→物理ch対応 (旧FITOM RhythmMapCh): {7,8,8,7,6}
//
//  sub-device自動生成により、OPLL本体と同一の物理ポートを共有する
//  独立デバイスとして生成される。
// ================================================================
class COPLLRhythm : public CSoundDevice {
public:
    COPLLRhythm(IPort* port, int /*sampleRate*/)
        : CSoundDevice(DEVICE_OPLL_RHY, 5, port,
                       kOpllMasterClock, 72,
                       FNUM_OFFSET,
                       FnumTableType::Fnumber,
                       0x40)
    {}

    std::string getDescriptor() const override { return "OPLL Rhythm 5ch"; }
    void init() override { setReg(0x0E, 0x20, true); }

protected:
    // パート(0-4: HH,CYM,TOM,SD,BD) → 音量レジスタ / 物理ch対応 (旧FITOM完全移植)
    static constexpr uint8_t kRhythmReg[5]   = {0x37, 0x38, 0x38, 0x37, 0x36};
    static constexpr uint8_t kRhythmMapCh[5] = {7, 8, 8, 7, 6};

    // リズムパートはROM内蔵の固定音色だが、Fnum/Blockレジスタの値
    // 自体はノート番号に応じて可変にする (ユーザー要望: ノート番号に
    // 対応したFnumを与えることで、内蔵ドラム音のピッチシフト演奏を
    // 可能にする)。基底クラスCSoundDevice::getFnumber()の標準的な
    // ノート→Fnum変換をそのまま使う(COPLLRhythmのコンストラクタで
    // 既にFnumTableType::Fnumber+FNUM_OFFSETが設定済みなので、
    // 通常のOPLL FMチャンネルと全く同じ変換になる)。
    // ch7(HH/SD共用)・ch8(CYM/TOM共用)は物理チャンネルを共有するが、
    // HH(ch0)・CYM(ch1)側がFnum更新自体を行わない(updateFreq参照)ため、
    // 実質的にSD/TOM側が各共有チャンネルのFnumを排他的に制御する形に
    // なる(2026年7月、ユーザー要望。HH/CYMはノイズ性の発振でFnumが
    // ほぼ発音に寄与しないため、後着で書き込むとSD/TOMのFnumを意図せず
    // 上書きしてしまっていた。以前はch7/ch8とも後着優先で上書きされる
    // 仕様だった)。
    void updateVoice(uint8_t ch) override {
        updateFreq(ch, nullptr);
        updateVolExp(ch);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        if (ch == 0 || ch == 1) return; // HH/CYM: Fnum更新を行わない(上記コメント参照)
        uint8_t vch = kRhythmMapCh[ch];
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint8_t b0cur = getReg(static_cast<uint16_t>(0x20 + vch)) & 0x30;
        setReg(static_cast<uint16_t>(0x10 + vch),
               static_cast<uint8_t>(fnum.fnum & 0xFF), true);
        setReg(static_cast<uint16_t>(0x20 + vch),
               static_cast<uint8_t>(b0cur | ((fnum.block & 7) << 1) | ((fnum.fnum >> 8) & 1)), true);
    }
    void updateSustain(uint8_t /*ch*/) override {}
    void updatePanpot(uint8_t /*ch*/) override {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        // MIDI Volume(CC#7)とベロシティの組み合わせ (Expressionは含まない)。
        // 旧FITOMはCalcLinearLevel(vev, tl)(第2引数が減衰量[高いほど静か])
        // を使い127-velocityで変換していたが、新しいcalcVolExpVel(vol, exp,
        // vel)は3引数とも同じラウドネス空間(高いほど大きい音、OPL4AWM/
        // PSGのcalcVolExpVel(s.volume, s.expression, s.velocity)呼び出しと
        // 同じ規約)のため、旧来の127-反転をそのまま持ち込むと強く叩くほど
        // 静かに、弱く叩くほど大きく鳴る逆転バグになっていた
        // (2026年8月、メロディパートのTL極性反転バグ修正を受けたユーザーの
        // 指摘でビルトインリズム側も確認し発覚)。
        uint8_t loudness = fitom::calcVolExpVel(s.volume, 127u, s.velocity);
        // STEP075DB (無マスク) で渡す理由は updateVolExp (トーンパート側) 参照
        uint8_t vol = fitom::linear2dB(loudness, RANGE48DB, STEP075DB, 4);
        uint16_t addr = kRhythmReg[ch];
        // ch&5: ch=0,2(HH/TOM)は上位nibble、ch=1,3,4(CYM/SD/BD)は下位nibble
        // (旧FITOM完全移植。2026年8月、判定が逆転しておりSDが自分では
        // $37下位に一度も書き込まれず起動直後は最大音量のままハイハット
        // 発音時にHHの音量で上書きされ以後固定的に減衰する等の不具合を
        // ユーザー指摘[レジスタダンプ比較]で修正。旧FITOM(legacy/src/
        // OPLL.cpp COPLLRhythm::UpdateVolExp)の`mask = (ch&5) ? 0xf0 : 0x0f`
        // (ch&5が真のとき下位nibble書き込み)と突き合わせて確認済み)。
        bool highNibble = (ch & 5) == 0;
        uint8_t mask = highNibble ? 0x0F : 0xF0;
        uint8_t shifted = highNibble ? static_cast<uint8_t>(vol << 4) : vol;
        setReg(addr, static_cast<uint8_t>((getReg(addr) & mask) | shifted));
    }

    void updateKey(uint8_t ch, bool keyOn) override {
        uint8_t keymask = static_cast<uint8_t>(~(1u << ch));
        uint8_t cur = getReg(0x0E) & keymask;
        setReg(0x0E, static_cast<uint8_t>(cur | 0x20 | (keyOn ? (1u << ch) : 0)), true);
    }

    // パート番号は音色データの hw.ALG (下位3bit) で直接指定する。
    // 該当パートが既に使用中なら 0xFF (旧FITOM COPLLRhythm::QueryCh 完全移植)。
    uint8_t queryCh(IMidiCh* /*owner*/, const HwPatch* patch, int mode) override {
        if (!patch) return 0xFF;
        uint8_t num = patch->hw.ALG & 0x7;
        if (num >= 5) return 0xFF;
        bool inuse = (getReg(0x0E) & (1u << num)) != 0;
        return mode ? num : (inuse ? 0xFF : num);
    }
};

} // namespace fitom

namespace fitom {
std::unique_ptr<ISoundDevice> createCOPLL(IPort* p, int sr, uint8_t m)  { return std::make_unique<COPLL>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLL2(IPort* p, int sr, uint8_t m) { return std::make_unique<COPLL2>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLLP(IPort* p, int sr, uint8_t m) { return std::make_unique<COPLLP>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCOPLLX(IPort* p, int sr, uint8_t m) { return std::make_unique<COPLLX>(p, sr, m); }
std::unique_ptr<ISoundDevice> createCVRC7(IPort* p, int sr)  { return std::make_unique<CVRC7>(p, sr); }
std::unique_ptr<ISoundDevice> createCOPLLRhythm(IPort* p, int sr) { return std::make_unique<COPLLRhythm>(p, sr); }

// ================================================================
//  フォールバック受け入れ判定
// ================================================================
// OPLLファミリー (COPLL/COPLLP/COPLLX/CVRC7、VOICE_PATCH_OPLL/OPLLP/
// OPLLX/VRC7) はユーザー音色 (ext.ALG_EXT&1==0) の場合のみ相互
// フォールバック可能。プリセット音色 (ALG_EXT&1==1) はROMデータが
// チップごとに全く異なる別音色のため不可。
// (COPLL2はVOICE_PATCH_OPLLをCOPLLと共有しているため対象外)
bool opllFamilyAcceptsFallback(uint8_t sourceVoicePatchType, uint8_t selfVoicePatchType,
                                const HwPatch& patch) {
    auto isOpllFamily = [](uint8_t v) {
        return v == VOICE_PATCH_OPLL || v == VOICE_PATCH_OPLLP
            || v == VOICE_PATCH_OPLLX || v == VOICE_PATCH_VRC7;
    };
    if (sourceVoicePatchType == selfVoicePatchType) return false; // 自分自身は対象外
    if (!isOpllFamily(sourceVoicePatchType)) return false;
    bool isPreset = (patch.ext.ALG_EXT & 1) != 0;
    return !isPreset;
}

} // namespace fitom
