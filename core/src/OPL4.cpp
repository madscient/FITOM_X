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
        setReg(0xF9, 0x00, true); // PCM出力レベル最大 (L/R共)
    }

protected:
    // OPL4 AWM(PCM)部のFnumber/Octaveは、OPN/OPM系FM合成のFnum位相累算器
    // (getFnumberFromHz()が実装するもの)とは全く別物である。実際は
    // ymfm(extern/ymfm/src/ymfm_pcm.cpp pcm_channel::cache_operator_data())が
    //   int32_t octave = int8_t(ch_octave(choffs) << 4) >> 4;  // 符号付き4bit(-8〜+7)
    //   step = ((0x400 | fnum) << (octave + 7)) >> 2;          // ROM上のバイトを
    //                                                           // 1出力サンプルあたり
    //                                                           // 何byte進めるか
    // という「ROMバイトの消費レートを、符号付きoctaveで表すオクターブと
    // 1024〜2047に正規化されたfnumで指数/仮数表現する」形式であり、
    // Octave(reg 0x38 bit7-4)は0〜7にクランプされる不動小数点ではなく
    // 符号付き4bit(2の補数)である(ALSAのsound/drivers/opl4/opl4_synth.c
    // snd_opl4_update_pitch()も`octave = pitch/0x600 - 8`と明示的に符号付き
    // 計算している)。
    //
    // さらに、ROM波形は「どのfnum/octaveで鳴らせば正しい絶対音高になるか」
    // が波形ごとに実測でしか分からない(ROM側にそれを示す情報が無いため)。
    // ALSAドライバはYRW801実機向けに実測したpitch_offset(100/128セント
    // 単位)・key_scaling(%)をwave_index(tone)ごとに埋め込んだ表
    // (sound/drivers/opl4/yrw801.c)を持ち、これをSampleZone::pitchOffset/
    // keyScalingとして移植してある(config/profiles/opl4awm_yrw801_*.
    // samplezonebank.json)。
    //
    // (2026年8月、ユーザー報告「AWMは音は出るが意図した波形と異なる」の
    // 調査で判明。以前はgetFnumberFromHz()[OPN用]を誤用しており、
    // 波形ごとの校正が全く反映されず、しかもoctaveが0〜7にクランプされて
    // 負のoctaveを一切表現できていなかった)。
    ChState::Fnum getFnumber(uint8_t ch, int16_t offset = 0) const override {
        ChState::Fnum ret;
        const auto& s = chState_[ch];
        if (s.lastNote >= 128) return ret;

        const SampleZone* zone = s.samplePatch
            ? s.samplePatch->resolveZone(s.lastNote, s.velocity) : nullptr;
        double keyScaling       = zone ? static_cast<double>(zone->keyScaling) / 100.0 : 1.0;
        double pitchOffset128th = zone ? static_cast<double>(zone->pitchOffset) : 0.0;

        // s.fineFreq/offset/チャンネルLFOは「1/64セント」単位(他チップと共通の
        // 規約)。ALSAと同じ「1/128半音」単位(100/128セント=0.78125セント)へ
        // 変換する: (totalOffset/64セント) * (1半音/100セント) * 128 = totalOffset*128/6400。
        int32_t totalOffset = static_cast<int32_t>(s.fineFreq) + offset
                             + (s.proc.channelLfoActive() ? s.proc.channelLfoValue() : 0);
        double fineOffset128th = static_cast<double>(totalOffset) * 128.0 / 6400.0;

        // ALSA snd_opl4_update_pitch()と同じ基準点(note=60, offset=0 →
        // pitch=60*128=7680)からの計算式。
        double pitch = (static_cast<double>(s.lastNote) - 60.0) * 128.0 * keyScaling
                     + 60.0 * 128.0
                     + pitchOffset128th
                     + fineOffset128th;
        pitch = std::clamp(pitch, 0.0, static_cast<double>(0x5fff));

        int32_t ipitch  = static_cast<int32_t>(std::llround(pitch));
        int32_t octave  = ipitch / 0x600 - 8;               // 符号付き(-8〜+7)
        int32_t within  = ipitch % 0x600;                   // オクターブ内位置(0〜1535)
        // snd_opl4_pitch_map[]と等価な閉形式(1024*(2^(x/1536)-1))。
        double fnumD = 1024.0 * (std::pow(2.0, static_cast<double>(within) / 1536.0) - 1.0);

        ret.fnum  = static_cast<uint16_t>(std::clamp<long long>(std::llround(fnumD), 0, 1023));
        // 2の補数4bitとしてそのまま使われる(updateFreq()の`fnum.block & 0xF`参照)。
        ret.block = static_cast<uint8_t>(octave);
        return ret;
    }

    // 波形番号bit8(reg 0x20)を、下位8bit(reg 0x08)より先に書く必要がある。
    // ymfm(pcm_engine::write())はreg 0x08-0x1F(波形番号下位8bit)への書き込みで
    // 即座にload_wavetable()をトリガーし、その時点のreg 0x20+ch bit0を上位
    // 1bitとして結合して波形テーブルヘッダを読みに行く。順序を逆にすると、
    // 新しい波形番号のbit8が反映される前(=1つ前のノートのbit8のまま)で
    // ロードが実行され、意図した波形と異なる(wavenumが256ずれた)波形が
    // 鳴ってしまう(2026年7月、ユーザー報告「音は出るが意図した波形でない」
    // により発覚。extern/ymfm/src/ymfm_pcm.cppのload_wavetable()呼び出し
    // 箇所と、OPN系F-number書き込み[reg 0xA4→0xA0の順、高位バイトを先に
    // 書いてから低位バイトで確定させる同種の設計]との比較で判明)。
    void updateVoice(uint8_t ch) override {
        const auto& s = chState_[ch];
        uint16_t waveNum = 0;
        if (const SampleZone* zone = s.samplePatch ? s.samplePatch->resolveZone(s.lastNote, s.velocity) : nullptr) {
            waveNum = zone->waveIndex;
        }
        uint8_t reg20cur = getReg(static_cast<uint16_t>(0x20 + ch)) & 0xFE;
        setReg(static_cast<uint16_t>(0x20 + ch),
               static_cast<uint8_t>(reg20cur | ((waveNum >> 8) & 1)), true);
        setReg(static_cast<uint16_t>(0x08 + ch), static_cast<uint8_t>(waveNum & 0xFF), true);
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

    // toneAttenuate(加算)・volumeFactor(「最大からの余白」への乗算、
    // 254=無補正)は波形ごとの音量校正値。ALSA snd_opl4_update_volume()
    // (sound/drivers/opl4/yrw801.cのopl4_sound::tone_attenuate/
    // volume_factor)と同じ規約で、CC#7/CC#11/velocity由来の減衰量
    // (calcVolExpVel、GM準拠のdBログ加算)に対して波形固有の補正を
    // 追加で適用する(2026年8月新設。GM楽器間の音量バランスを取るための
    // 校正値で、無いと一部楽器が相対的に大きすぎ/小さすぎに聞こえる)。
    void updateVolExp(uint8_t ch) override {
        const auto& s = chState_[ch];
        const SampleZone* zone = s.samplePatch
            ? s.samplePatch->resolveZone(s.lastNote, s.velocity) : nullptr;
        uint8_t loudness = fitom::calcVolExpVel(s.volume, s.expression, s.velocity);
        int totalLevel = 127 - static_cast<int>(loudness); // 7bit、大きいほど減衰
        if (zone) {
            totalLevel += zone->toneAttenuate;
            totalLevel = 127 - (127 - totalLevel) * zone->volumeFactor / 254;
        }
        totalLevel = std::clamp(totalLevel, 0, 127);
        setReg(static_cast<uint16_t>(0x50 + ch),
               static_cast<uint8_t>((static_cast<uint8_t>(totalLevel) & 0x7F) << 1), false); // LevelDirect=0
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
