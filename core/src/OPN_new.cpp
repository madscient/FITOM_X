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
    COPN(IPort* port, uint8_t deviceId = DEVICE_OPN, int fnumMaster = 3993600)
        : CSoundDevice(deviceId, 3, port,
                       fnumMaster, 144,
                       -576,
                       FnumTableType::Fnumber,
                       256)
    {
        opCount_ = 4;
    }

    std::string getDescriptor() const override {
        return "OPN (YM2203) 3ch";
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

        updatePanpot(ch);
    }

    // ──────────────────────────────────────────────────────────────
    //  UpdateFreq: F-number をチップに書き込む
    // ──────────────────────────────────────────────────────────────
    void updateFreq(uint8_t ch, const ChState::Fnum* fn = nullptr) override {
        if (ch >= 3) return;
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        // MSB 書き込み後に LSB 書き込み (OPN の仕様)
        setReg(static_cast<uint16_t>(0xA4 + ch),
               (fnum.block << 3) | ((fnum.fnum >> 8) & 0x7));
        setReg(static_cast<uint16_t>(0xA0 + ch),
               fnum.fnum & 0xFF);
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
std::unique_ptr<ISoundDevice> createSubCOPN_impl(IPort* port, int fnumMaster) {
    return std::make_unique<COPN>(port, DEVICE_OPN, fnumMaster);
}

std::unique_ptr<ISoundDevice> createCOPN(IPort* p, int sr) {
    return std::make_unique<COPN>(p);
}
} // namespace fitom
