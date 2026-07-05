// fitom/OPL4.cpp
// YMF278 (OPL4) の AWM(PCM)部ドライバ実装。
//
// OPL4はFM部(OPL3完全互換, YMF278内蔵)とAWM部(PCM/波形メモリ, YRW801外付け
// またはYMF278内蔵ROM連携)を持つ複合チップ。FM部は OPL_new.cpp の COPL3 が
// (Config::resolveCompositeSpec 経由の sub-device 自動生成で) 別途担当する。
// 本ファイルは AWM部 (COPL4AWM) のみを扱う。

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include "fitom/VolumeUtils.h"
#include "fitom/Log.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fitom {

// ================================================================
//  COPL4AWM: YMF278 (OPL4) の AWM(PCM)部。YRW801内蔵ROM音色専用。
//  CSoundDeviceを直接継承する (CAdPcmBaseは継承しない)。ROM音色のみを
//  扱うため、CAdPcmBaseが提供するユーザーPCMロード機構(loadVoice/
//  PcmBankRegistry/maxMem等)を一切使わず、恩恵がないため。
//
//
//  レジスタマップ (ユーザー提供のYMF278アプリケーションノート画像で確定):
//    0x08-0x1F: 波形番号 下位8bit (ch0-23)
//    0x20-0x37: F_NUMBER下位7bit(bit7-1) | 波形番号bit8(bit0)
//    0x38-0x4F: Octave(bit7-4) | PseudoReverb(bit3) | F_NUMBER上位3bit(bit2-0)
//    0x50-0x67: TotalLevel(bit7-1) | LevelDirect(bit0)
//    0x68-0x7F: KEYON(bit7) | DAMP(bit6) | LFORST(bit5) | CH(bit4,出力ピン選択,常に0) | Panpot(bit3-0,2の補数符号付き4bit,0=中央)
//    0x80-0x97: LFO(bit6-4) | VIB(bit2-0)
//    0x98-0xAF: AR(bit7-4) | D1R(bit3-0)
//    0xB0-0xC7: DL(bit7-4) | D2R(bit3-0)
//    0xC8-0xDF: RateCorrection(bit7-4) | RR(bit3-0)
//    0xE0-0xF7: AM(bit2-0)
//    0xF8: Mixing FM_R(bit7-4)/FM_L(bit3-0)
//    0xF9: Mixing PCM_R(bit7-4)/PCM_L(bit3-0)
//
//  当面はYRW801内蔵ROM音色のみサポート (GM Level1音源として振る舞う)。
//  音色選択は VOICE_PATCH_AWM 用の SampleZonePatch (プロファイル
//  config/profiles/opl4awm_yrw801_gm.samplezonebank.json 等からロード)
//  経由で行う。ChState::samplePatch のキーゾーン (+ベロシティレイヤー)
//  をノート/ベロシティで線形探索し、該当する SampleZone::waveIndex を
//  実際のROM波形番号としてレジスタに書く。
//  (旧: hwOp[0].WSにGM Program Number/ドラムノート番号を詰め、
//   コード内蔵のkAllRegions[610]定数テーブルを検索する設計だったが、
//   YRW801以外のROM/カスタムサンプルセットへの対応、ADPCM系への
//   スキーマ再利用を見据えてプロファイル化した)。
//  ユーザーPCMのロード(loadVoice)は非対応 (ROM専用)。
// ================================================================
class COPL4AWM : public CSoundDevice {
public:
    COPL4AWM(IPort* port, int sampleRate)
        : CSoundDevice(DEVICE_OPL4AWM, 24, port,
                       sampleRate, 0,
                       FNUM_OFFSET, FnumTableType::None, 0x100)
    {
        opCount_ = 1;
    }

    std::string getDescriptor() const override { return "OPL4 AWM (YRW801 ROM) 24ch"; }

    void init() override {
        setReg(0xF8, 0x00, true); // FM出力ミキサーはCOPL3側が別途担当するためここでは0
        setReg(0xF9, 0x3F, true); // PCM出力レベル最大 (L/R共)
    }

protected:
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        // OPL4のFnumberは10bit・Octave4bitのOPN/OPM系に近い形式。
        // CAdPcmBaseが強制するFnumTableType::DeltaN用テーブルは使わず、
        // OPL3と同じ11bit精度計算式を直接使う (getFnumberFromHz等と同系統)。
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;
        int32_t totalOffset = static_cast<int32_t>(s.fineFreq) + offset
                             + (s.proc.channelLfoActive() ? s.proc.channelLfoValue() : 0);
        double semitone = (static_cast<double>(s.lastNote) - 69.0)
                         + static_cast<double>(totalOffset) / 64.0 / 100.0;
        double hz = 440.0 * std::pow(2.0, semitone / 12.0);
        return getFnumberFromHz(hz);
    }

    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        uint16_t waveNum = 0;
        if (s.samplePatch && !s.samplePatch->zones.empty()) {
            bool found = false;
            for (const auto& z : s.samplePatch->zones) {
                if (s.lastNote >= z.keyMin && s.lastNote <= z.keyMax &&
                    s.velocity >= z.velMin && s.velocity <= z.velMax) {
                    waveNum = z.waveIndex;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // 該当ゾーンが見つからない場合は最初のゾーンにフォールバック
                // (旧resolveWaveNumber()の挙動と同じ)
                waveNum = s.samplePatch->zones[0].waveIndex;
            }
        }
        setReg(static_cast<uint16_t>(0x08 + ch), static_cast<uint8_t>(waveNum & 0xFF), true);
        uint8_t reg20cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0xFE;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(reg20cur | ((waveNum >> 8) & 1)), true);
        updateVolExp(ch);
        updatePanpot(ch);
    }

    void updateFreq(uint8_t ch, const ChState::Fnum* fn) override {
        ChState::Fnum fnum = fn ? *fn : getFnumber(ch);
        uint16_t fn10 = fnum.fnum & 0x3FF; // Fnumberは10bit
        uint8_t reg20cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0x01; // 波形番号bit8保持
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(reg20cur | ((fn10 & 0x7F) << 1)), true);
        uint8_t reg38cur = getReg(static_cast<uint16_t>(0x38 + ch)) & 0x08; // PseudoReverb保持
        setReg(static_cast<uint16_t>(0x38 + ch),
               static_cast<uint8_t>(((fnum.block & 0xF) << 4) | reg38cur | ((fn10 >> 7) & 0x07)),
               true);
    }

    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        uint8_t loudness = fitom::calcVolExpVel(s.volume, s.expression, s.velocity);
        uint8_t totalLevel = 127u - loudness; // 7bit、大きいほど減衰
        setReg(static_cast<uint16_t>(0x50 + ch),
               static_cast<uint8_t>((totalLevel & 0x7F) << 1), false); // LevelDirect=0
    }

    void updatePanpot(uint8_t ch) override {
        int8_t pan = chState_[ch].panpot; // -64..63
        // 4bit符号付き2の補数表現 (-7〜+7、-8は未使用、0=中央)。
        // 正=右パン(L側が3dB/stepで減衰)、負=左パン(R側が3dB/stepで減衰)。
        // (実機データシート記載の Panpot テーブルに準拠)
        int p7 = std::clamp((static_cast<int>(pan) * 7) / 63, -7, 7);
        uint8_t pan4 = static_cast<uint8_t>(p7 & 0xF); // 負値は自動的に2の補数表現になる
        uint8_t cur = getReg(static_cast<uint16_t>(0x68 + ch)) & 0xF0;
        setReg(static_cast<uint16_t>(0x68 + ch),
               static_cast<uint8_t>(cur | pan4), false);
    }

    void updateSustain(uint8_t /*ch*/) override {}
    void updateTL(uint8_t, uint8_t, uint8_t) override {}

    // KEYON(bit7)/DAMP(bit6)/LFORST(bit5)/CH(bit4,常に0=FMとミックス)を制御。
    // ノートオン時: KEYON=1, DAMP=0, LFORST=1 (LFO波形をリセットしてから開始)。
    // ノートオフ時: KEYON=0 (DAMP/LFORSTも0に戻す)。
    void updateKey(uint8_t ch, bool keyOn) override {
        uint8_t cur = getReg(static_cast<uint16_t>(0x68 + ch)) & 0x0F; // Panpot保持
        uint8_t bits = keyOn ? (0x80 | 0x20) : 0x00; // KEYON|LFORST or 全クリア
        setReg(static_cast<uint16_t>(0x68 + ch),
               static_cast<uint8_t>(cur | bits), true);
    }

    // 強制ダンプ (CC#120 All Sound Off等)。DAMPビット(bit6)を追加するだけで、
    // 現在のKEYON/Panpot状態は変更しない (データシート: "decay stateでDAMP=1にする"
    // という記述に準拠、既存状態への上書きではなく追加のビットとして扱う)。
    void forceDamp(uint8_t ch) override {
        if (ch >= maxChs_) return;
        uint8_t cur = getReg(static_cast<uint16_t>(0x68 + ch));
        setReg(static_cast<uint16_t>(0x68 + ch),
               static_cast<uint8_t>(cur | 0x40), true);
        noteOff(ch);
    }
};

// ================================================================
//  ファクトリ関数
// ================================================================
std::unique_ptr<ISoundDevice> createCOPL4AWM(IPort* p, int sr) {
    return std::make_unique<COPL4AWM>(p, sr);
}

} // namespace fitom
