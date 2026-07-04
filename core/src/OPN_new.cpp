// fitom/OPN_new.cpp
// COPN チップドライバ — ISoundDevice ベースへの移行版
//
// 移行パターン:
//   旧: FMVOICE* voice = attr->GetVoice() → 各フィールドを直接読む
//   新: const HwPatch& p = s.hwPatch → hw.FB/ALG, hwOp[i].AR 等を読む
//       TL は s.proc.effectiveTL(op) を読む (VolExp/LFO 適用済み)
//
// このファイルは OPN.cpp 全体のリファクタリング版。
// 他のチップドライバも同じパターンで移行する:
//   OPM_new.cpp / OPL_new.cpp / OPLL_new.cpp / SSG_new.cpp 等

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/Log.h"

namespace fitom {

// ================================================================
//  COPN クラス定義
// ================================================================
class COPN : public CSoundDevice {
public:
    // fnumMaster: チップのマスタークロック [Hz]。
    // 単体 OPN (YM2203) はデフォルト 3.993MHz。
    // OPN2/OPNA 系のサブチップとして使用する場合は
    // 親クラスのコンストラクタから適切なクロック値を渡す。
    //
    // fxCapable: FXモード(3rd channel special mode)対応の有無。
    // 実機OPN系は「チップの ch2 (最初の3ch分の3番目)」のみがFXモード対応で、
    // OPNA/OPN2 の後半3ch分 (chip2_、port2側) には対応するレジスタが
    // 実機に存在しない。単体OPNおよびOPNA/OPN2の前半サブチップはtrue、
    // OPNA/OPN2の後半サブチップはfalseを渡す (旧FITOMのfxenaフラグに相当)。
    COPN(IPort* port, uint8_t deviceId = DEVICE_OPN, int fnumMaster = 3993600,
         bool fxCapable = true)
        : CSoundDevice(deviceId, 3, port,
                       fnumMaster, 144,
                       -576,
                       FnumTableType::Fnumber,
                       256)
        , fxCapable_(fxCapable)
    {
        opCount_ = 4;
    }

    std::string getDescriptor() const override {
        return "OPN (YM2203) 3ch";
    }

    // FXモード(ch2専用)を要求するパッチは、ch2以外では正常動作しないため
    // 必ずch2を払い出す (旧FITOM COPN::QueryCh 相当)。
    // fxCapable_=false (OPNA/OPN2後半サブチップ) の場合はこの判定自体を行わない。
    uint8_t queryCh(IMidiCh* owner, const HwPatch* patch, int mode) override {
        if (fxCapable_ && patch && patch->ext.DM0 != 0) {
            const auto& s2 = chState_[2];
            bool avail = mode ? s2.isEmpty() : s2.isEnabled();
            return avail ? 2 : 0xFF;
        }
        return CSoundDevice::queryCh(owner, patch, mode);
    }

    void init() override {
        reset();
        // 初期化レジスタシーケンス
        setReg(0x27, 0x30, true); // ch3 normal mode, timer stop
        setReg(0x29, 0x80, true); // FM enable
    }

    void reset() override {
        CSoundDevice::reset();
        // 全レジスタクリア
        for (int i = 0x30; i < 0xB0; ++i) setReg(static_cast<uint16_t>(i), 0, true);
        for (int ch = 0; ch < 3; ++ch) setReg(static_cast<uint16_t>(0xB4 + ch), 0xC0, true); // L+R
    }

protected:
    // ──────────────────────────────────────────────────────────────
    //  UpdateVoice: HwPatch のレジスタイメージをチップに書き込む
    // ──────────────────────────────────────────────────────────────
    void updateVoice(uint8_t ch) override {
        if (ch >= 3) return;
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;

        // FB / ALG
        setReg(static_cast<uint16_t>(0xB0 + ch),
               ((p.hw.FB & 7) << 3) | (p.hw.ALG & 7));

        // AMS/PMS (OPN は実質未使用 → 0)
        // setReg(0xB4 + ch, ...);  // パンポット設定と合わせて updatePanpot で行う

        // 4 オペレータ
        for (int op = 0; op < 4; ++op) {
            uint8_t reg = static_cast<uint8_t>(opSlot(ch, op));
            const FmHwOp& o = p.hwOp[op];

            // DT1 / MUL
            setReg(static_cast<uint16_t>(0x30 + reg),
                   ((o.DT1 & 7) << 4) | (o.MUL & 0xF));

            // キャリア判定 (TL・EGレート両方で使う)
            const bool car = isCarrier(ch, op);

            // TL (キャリアは effectiveTL を使用、モジュレータは固定)
            const uint8_t tl = car
                ? s.proc.effectiveTL(op)
                : o.TL;
            setReg(static_cast<uint16_t>(0x40 + reg), tl & 0x7F);

            // KSR / AR (キャリアはベロシティ補正値を使用)
            const uint8_t ar = car ? s.proc.velAR(op) : (o.AR & 0x1F);
            setReg(static_cast<uint16_t>(0x50 + reg),
                   ((o.KSR & 3) << 6) | ar);

            // AM / DR (FmHwOp では DR = OPN の "DR" = OPM の "D1R")
            const uint8_t dr = car ? s.proc.velDR(op) : (o.DR & 0x1F);
            setReg(static_cast<uint16_t>(0x60 + reg),
                   ((o.AM & 1) << 7) | dr);

            // SR (FmHwOp では SR = OPN の "SR" = OPM の "D2R")
            const uint8_t sr = car ? s.proc.velSR(op) : (o.SR & 0x1F);
            setReg(static_cast<uint16_t>(0x70 + reg), sr);

            // SL / RR (FmHwOp では SL = OPN の "SL" = OPM の "D1L")
            const uint8_t sl = car ? s.proc.velSL(op) : (o.SL & 0xF);
            const uint8_t rr = car ? s.proc.velRR(op) : (o.RR  & 0xF);
            setReg(static_cast<uint16_t>(0x80 + reg),
                   ((sl & 0xF) << 4) | (rr & 0xF));

            // SSG-EG (EGT)
            setReg(static_cast<uint16_t>(0x90 + reg), o.EGT & 0xF);
        }

        // FXモード (3rd channel special mode): ch2専用、fxCapable_なチップのみ。
        // ext.DM0 (0=通常/1=疑似デチューン/2=非整数倍率/3=固定周波数) で
        // モードを選択する。0以外ならFXモード有効(0x27 bit7)。
        if (fxCapable_ && ch == 2) {
            uint8_t cur = getReg(0x27) & 0x7F;
            setReg(0x27, static_cast<uint8_t>(cur | (p.ext.DM0 != 0 ? 0x80 : 0)), true);
        }

        updatePanpot(ch);
    }

    // ──────────────────────────────────────────────────────────────
    //  UpdateFreq: F-number をチップに書き込む
    // ──────────────────────────────────────────────────────────────
    void updateFreq(uint8_t ch, const ChState::Fnum* fn = nullptr) override {
        if (ch >= 3) return;

        // FXモード有効時 (ch2かつfxCapable_のみ): 4オペレータ独立のFnumberを書く
        const HwPatch& p = chState_[ch].hwPatch;
        if (fxCapable_ && ch == 2 && p.ext.DM0 != 0 && !fn) {
            updateFxModeFreq(ch, p);
            return;
        }

        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // MSB 書き込み後に LSB 書き込み (OPN の仕様)
        setReg(static_cast<uint16_t>(0xA4 + ch),
               (fnum.block << 3) | ((fnum.fnum >> 8) & 0x7));
        setReg(static_cast<uint16_t>(0xA0 + ch),
               fnum.fnum & 0xFF);
    }

    // FXモード時、4オペレータそれぞれに独立したFnumberを書く。
    // レジスタ対応 (実機仕様): hwOp[0](Slot1)→0xA9/AD, hwOp[1](Slot3)→0xA8/AC,
    //                          hwOp[2](Slot2)→0xAA/AE, hwOp[3](Slot4)→0xA2/A6(通常ch2兼用)
    void updateFxModeFreq(uint8_t ch, const HwPatch& p) {
        static constexpr uint8_t kFnumLoReg[4] = {0xA9, 0xA8, 0xAA, 0xA2};
        static constexpr uint8_t kFnumHiReg[4] = {0xAD, 0xAC, 0xAE, 0xA6};

        for (int op = 0; op < 4; ++op) {
            const FmHwOp& o = p.hwOp[op];
            ChState::Fnum fnum;
            switch (p.ext.DM0) {
            case 1: // 疑似デチューン
            case 2: // 非整数倍率 (どちらもセントオフセットとして加算)
                fnum = getFnumber(ch, o.FXV);
                break;
            case 3: // 固定周波数 (0.1Hz単位)
                fnum = getFnumberFromHz(static_cast<double>(o.FXV) / 10.0);
                break;
            default:
                fnum = getFnumber(ch);
                break;
            }
            setReg(kFnumHiReg[op], static_cast<uint8_t>((fnum.block << 3) | ((fnum.fnum >> 8) & 0x7)));
            setReg(kFnumLoReg[op], static_cast<uint8_t>(fnum.fnum & 0xFF));
        }
    }

    // ──────────────────────────────────────────────────────────────
    //  UpdateVolExp: キャリア TL を VoiceProcessor 経由で再書き込み
    // ──────────────────────────────────────────────────────────────
    void updateVolExp(uint8_t ch) override {
        if (ch >= 3) return;
        const auto& s = chState_[ch];
        for (int op = 0; op < 4; ++op) {
            if (!isCarrier(ch, op)) continue;
            uint8_t tl = s.proc.effectiveTL(op);
            setReg(static_cast<uint16_t>(0x40 + opSlot(ch, op)), tl & 0x7F);
        }
    }

    // ──────────────────────────────────────────────────────────────
    //  UpdateTL: 特定オペレータの TL のみ更新 (LFO タイマー用)
    // ──────────────────────────────────────────────────────────────
    void updateTL(uint8_t ch, uint8_t op, uint8_t tl) override {
        if (ch >= 3 || op >= 4) return;
        setReg(static_cast<uint16_t>(0x40 + opSlot(ch, op)), tl & 0x7F);
    }

    // ──────────────────────────────────────────────────────────────
    //  UpdatePanpot: L/R フラグを B4 レジスタに書く
    // ──────────────────────────────────────────────────────────────
    void updatePanpot(uint8_t ch) override {
        if (ch >= 3) return;
        // OPN: pan は L/R 1bit のみ。panpot_ -64..+63 を L=0x80, R=0x40, C=0xC0 に変換
        uint8_t lr = 0xC0; // デフォルト: 両方
        int8_t pan = chState_[ch].panpot;
        if (pan < -20)      lr = 0x80; // L only
        else if (pan > 20)  lr = 0x40; // R only
        setReg(static_cast<uint16_t>(0xB4 + ch), lr);
    }

    // CC#120 (All Sound Off): 全 OP の RR を最大値にして急速減衰させてから noteOff。
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        const auto& s = chState_[ch];
        if (!s.isActive()) return;
        const HwPatch& p = s.hwPatch;
        for (int op = 0; op < 4; ++op) {
            if (!isCarrier(ch, op)) continue; // モジュレータは対象外
            const uint8_t sl = p.hwOp[op].SL & 0xF;
            setReg(static_cast<uint16_t>(0x80 + kOpMap[op] + ch),
                   static_cast<uint8_t>((sl << 4) | 0xF)); // RR=15 (最大)
        }
        noteOff(ch);
    }

    void updateSustain(uint8_t ch) override {
        // Sustain ON: キャリア OP の RR を 4 固定にして音を引き延ばす
        // Sustain OFF: 音色の RR に戻す (モジュレータは対象外)
        // 旧 FITOM COPN::UpdateSustain と同等 (REV パラメータは新FITOMにないため固定値 4)
        const auto& s = chState_[ch];
        const HwPatch& p = s.hwPatch;
        for (int op = 0; op < 4; ++op) {
            if (!isCarrier(ch, op)) continue; // モジュレータは対象外
            const FmHwOp& o = p.hwOp[op];
            const uint8_t rr = s.sustain ? 4u : (o.RR & 0xF);  // RR は 4bit
            const uint8_t sl = o.SL & 0xF;
            setReg(static_cast<uint16_t>(0x80 + kOpMap[op] + ch),
                   static_cast<uint8_t>((sl << 4) | (rr & 0xF)));
        }
    }

    // ──────────────────────────────────────────────────────────────
    //  UpdateKey: キーオン/オフ
    // ──────────────────────────────────────────────────────────────
    void updateKey(uint8_t ch, bool keyOn) override {
        if (ch >= 3) return;
        // リリース中の音が残った状態で同一chに新規ノートオンすると
        // アタック波形が不正になるため、事前に強制ダンプ(RR最大化)する。
        if (keyOn && chState_[ch].wasReleasing) {
            const HwPatch& p = chState_[ch].hwPatch;
            for (int op = 0; op < 4; ++op) {
                if (!isCarrier(ch, op)) continue;
                const uint8_t sl = p.hwOp[op].SL & 0xF;
                setReg(static_cast<uint16_t>(0x80 + kOpMap[op] + ch),
                       static_cast<uint8_t>((sl << 4) | 0xF)); // RR=15
            }
        }
        // OPN: 0x28 レジスタ、スロットマスクは 0xF0 (全スロット ON)
        uint8_t data = keyOn ? (0xF0 | (ch & 3)) : (ch & 3);
        setReg(0x28, data, true);
    }

private:
    // オペレータ → レジスタオフセット変換
    // OPN: ch 0/1/2 、op 0/1/2/3 → slot offset
    static const uint8_t kOpMap[4]; // = {0, 8, 4, 12}
    bool fxCapable_; // FXモード(3rd channel special mode)対応の有無

    uint8_t opSlot(uint8_t ch, int op) const {
        return static_cast<uint8_t>(kOpMap[op] + ch);
    }

    bool isCarrier(uint8_t ch, int op) const {
        uint8_t alg = chState_[ch].hwPatch.hw.ALG & 7;
        return (kCarrierMask[alg] >> op) & 1;
    }
};

const uint8_t COPN::kOpMap[4] = {0, 8, 4, 12};

} // namespace fitom

// ================================================================
//  ファクトリ関数 (DeviceFactory.cpp から呼ばれる)
// ================================================================
namespace fitom {

// OPN2_new.cpp の COPNA/COPN2 がサブチップとして COPN を生成するための関数。
// (OPN2_new.cpp は OPN_new.cpp の COPN 定義に依存するが、
//  ヘッダで公開すると循環インクルードが起きるため、
//  名前でリンク解決する間接ファクトリを使う)
std::unique_ptr<ISoundDevice> createSubCOPN_impl(IPort* port, int fnumMaster, bool fxCapable) {
    return std::make_unique<COPN>(port, DEVICE_OPN, fnumMaster, fxCapable);
}

std::unique_ptr<ISoundDevice> createCOPN(IPort* p, int sr) {
    return std::make_unique<COPN>(p);
}

// ================================================================
//  フォールバック受け入れ判定 (DeviceFactory::acceptsFallback から呼ばれる)
// ================================================================
// COPN (VOICE_PATCH_OPN): OPN2形式の音色データをそのまま再生できるか。
// 同一FMコアのためレジスタレベルで完全互換 (ch4-6/FXモード等の拡張は
// 単にOPN(3ch)側では使われないだけで、データの解釈自体は変わらない)。
bool copnAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& /*patch*/) {
    return sourceVoicePatchType == VOICE_PATCH_OPN2;
}

// COPNA/COPN2 (VOICE_PATCH_OPN2): OPN形式の音色データを再生できるか。
// 同様に完全互換。
bool copn2AcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& /*patch*/) {
    return sourceVoicePatchType == VOICE_PATCH_OPN;
}

} // namespace fitom
