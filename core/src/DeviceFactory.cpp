// fitom/DeviceFactory.cpp
// IPort → ISoundDevice ファクトリ実装

#include "fitom/DeviceFactory.h"
#include "fitom/Config.h"
#include "fitom/Log.h"
#include <memory>

// ================================================================
//  ファクトリ関数の前方宣言 (各 *_new.cpp で定義)
// ================================================================
namespace fitom {

std::unique_ptr<ISoundDevice> createCOPN(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPNA(IPort* p, int masterClock, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPNB(IPort* p, int masterClock, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPN2(IPort* p, int masterClock, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPN2C(IPort* p, int masterClock, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPN2L(IPort* p, int masterClock, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPNARhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPM(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPP(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPZ(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPL(IPort* p, int masterClock, bool rhythmMode = false);
std::unique_ptr<ISoundDevice> createC3801(IPort* p, int masterClock, bool rhythmMode = false);
std::unique_ptr<ISoundDevice> createCOPL2(IPort* p, int masterClock, bool rhythmMode = false);
std::unique_ptr<ISoundDevice> createCOPL2EX(IPort* p, int masterClock, bool rhythmMode = false);
std::unique_ptr<ISoundDevice> createCOPL3(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPL3_2(IPort* p, int masterClock, bool rhythmMode = false);
std::unique_ptr<ISoundDevice> createCOPLL(IPort* p, int masterClock, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLL2(IPort* p, int masterClock, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLP(IPort* p, int masterClock, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLX(IPort* p, int masterClock, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLEX(IPort* p, int masterClock, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLRhythm(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPLLEXRhythm(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPLRhythm(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCOPL2EXRhythm(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCVRC7(IPort* p, int masterClock);
std::unique_ptr<ISoundDevice> createCSSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCSSGS(IPort* p, int sr, uint32_t deviceType);
std::unique_ptr<ISoundDevice> createCEPSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCDCSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCDSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCDSGRhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCSCC(IPort* p, int sr, uint32_t deviceType);
std::unique_ptr<ISoundDevice> createCSAA1099(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCAdPcm(IPort* p, int sr, uint32_t deviceType);
std::unique_ptr<ISoundDevice> createCOPL4AWM(IPort* p, int sr);

// フォールバック受け入れ判定関数 (各チップドライバファイルで定義)
bool copnAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool copn2AcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool copmAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool copzAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool copz2AcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool coplAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool copl2AcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool cssgAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool cepsgAcceptsFallback(uint8_t sourceVoicePatchType, const HwPatch& patch);
bool opllFamilyAcceptsFallback(uint8_t sourceVoicePatchType, uint8_t selfVoicePatchType,
                                const HwPatch& patch);

// ================================================================
//  DeviceFactory::create
// ================================================================
// 音程・再生レートは「チップのクロック÷分周」で決まるため、音声出力の
// サンプルレート(sampleRate)ではなく、ポートが報告するチップのクロック
// (IPort::getClock()。エミュレーターならプラグイン設定のclock)を使う。
// clockDivider/clockMultiplierは、そのクロックをサブデバイスのドライバが
// 計算に使うクロックへ換算する比(DeviceEntry::clockDivider参照)。
static int scaleClock(int clock, int clockDivider, int clockMultiplier)
{
    if (clockDivider < 1) clockDivider = 1;
    if (clockMultiplier < 1) clockMultiplier = 1;
    return static_cast<int>(static_cast<long long>(clock) * clockMultiplier / clockDivider);
}

// FM系: ポートがクロックを報告しないとき(0)は0を返し、ドライバが持つ
// 標準クロックを使わせる。
static int fmMasterClock(IPort* port, int clockDivider, int clockMultiplier)
{
    const int clock = port ? port->getClock() : 0;
    return (clock > 0) ? scaleClock(clock, clockDivider, clockMultiplier) : 0;
}

// PSG系: クロックを取得できない場合のみsampleRateへフォールバックする
// (createCAdPcm()と同じ)。
static int psgMasterClock(IPort* port, int sampleRate, int clockDivider,
                          int clockMultiplier)
{
    int clock = port ? port->getClock() : 0;
    if (clock <= 0) clock = sampleRate;
    return scaleClock(clock, clockDivider, clockMultiplier);
}

// SSGS系 (YMZ705/YMZ732) の SSG ブロック動作クロック。プロファイルに
// 書かれるのはチップのマスタークロックだが、実チップはどの構成でも
// SSG ブロックを 2.048MHz で動かす:
//   YMZ705 (SSGS) : 4.096MHz を1/2、または 6.144MHz を1/3 (S6Mピンで選択)
//   YMZ732 (SSGS2): 12.288MHz を1/6 (クロックは1択。S6Mピンの役割自体が
//                   CPUインターフェイスモード選択へ変わっている)
//   YMZ771 (SSGS3): 16.384MHz を1/8 (データシートにSSGブロックの動作
//                   クロックの記載は無いが、この分周比がファミリ共通の
//                   2.048MHzと一致する)
// 分周比がクロック値そのものに依存するため、固定値の clockDivider
// (kOpnSsgClockDivider のような定数) では表現できず、ここで判定する。
// YMZ705の境界値は2つの規定値の中点。エミュレーションエンジン側
// (EPSGemuEngine の ssgsInternalClock) と同じ規則にすること。
static int ssgsSsgBlockClock(uint32_t deviceType, IPort* port, int sampleRate,
                              int clockDivider, int clockMultiplier)
{
    int clock = psgMasterClock(port, sampleRate, clockDivider, clockMultiplier);
    if (deviceType == DEVICE_SSGS3) return clock / 8;
    if (deviceType == DEVICE_SSGS2) return clock / 6;
    return clock / ((clock >= 5120000) ? 3 : 2);
}

std::unique_ptr<ISoundDevice> DeviceFactory::create(
    uint32_t deviceType, IPort* port, int sampleRate, IPort* extraPort, bool rhythmMode,
    int clockDivider, int clockMultiplier)
{
    if (!port) {
        FITOM_LOG_ERR("DeviceFactory::create: port is null for device 0x"
            << std::hex << deviceType);
        return nullptr;
    }

    const uint8_t mode = rhythmMode ? 1 : 0;
    const int fmClock = fmMasterClock(port, clockDivider, clockMultiplier);

    switch (deviceType) {
    case DEVICE_OPN:
    case DEVICE_OPNC:
        return createCOPN(port, fmClock);

    case DEVICE_OPNB:
        // YM2610無印: COPNA相当の6ch構成からch0/ch3を無効化した実効4ch
        // (COPNB参照)。extraPort が nullptr の場合 COPNA 内部で
        // OffsetPort を生成する。
        return createCOPNB(port, fmClock, extraPort);

    case DEVICE_OPNA:
    case DEVICE_2610B:
    case DEVICE_F286:
    case DEVICE_OPN3:
        // extraPort が nullptr の場合 COPNA 内部で OffsetPort を生成する
        return createCOPNA(port, fmClock, extraPort);

    case DEVICE_OPN2:
        return createCOPN2(port, fmClock, extraPort);
    case DEVICE_OPN2C:
        return createCOPN2C(port, fmClock, extraPort);
    case DEVICE_OPN2L:
        return createCOPN2L(port, fmClock, extraPort);

    case DEVICE_OPNA_RHY:
        return createCOPNARhythm(port, sampleRate);

    case DEVICE_OPM:       return createCOPM(port, fmClock);
    case DEVICE_OPP:       return createCOPP(port, fmClock);
    case DEVICE_OPZ:
    case DEVICE_OPZ2:      return createCOPZ(port, fmClock);

    case DEVICE_OPL:       return createCOPL(port, fmClock, rhythmMode);
    case DEVICE_Y8950:     return createC3801(port, fmClock, rhythmMode);
    case DEVICE_OPL2:      return createCOPL2(port, fmClock, rhythmMode);
    case DEVICE_OPL2EX:    return createCOPL2EX(port, fmClock, rhythmMode);
    case DEVICE_OPL3:
    case DEVICE_OPN3_L3:   return createCOPL3(port, fmClock);
    case DEVICE_OPL3_2:    return createCOPL3_2(port, fmClock, rhythmMode);
    case DEVICE_OPL_RHY:   return createCOPLRhythm(port, fmClock);
    case DEVICE_OPL2EX_RHY: return createCOPL2EXRhythm(port, fmClock);

    case DEVICE_OPLL:      return createCOPLL(port, fmClock, mode);
    case DEVICE_OPLL2:     return createCOPLL2(port, fmClock, mode);
    case DEVICE_OPLLP:     return createCOPLLP(port, fmClock, mode);
    case DEVICE_OPLLX:     return createCOPLLX(port, fmClock, mode);
    case DEVICE_OPLLEX:    return createCOPLLEX(port, fmClock, mode);
    case DEVICE_VRC7:      return createCVRC7(port, fmClock);
    case DEVICE_OPLL_RHY:  return createCOPLLRhythm(port, fmClock);
    case DEVICE_OPLLEX_RHY: return createCOPLLEXRhythm(port, fmClock);

    case DEVICE_SSG:
    case DEVICE_PSG:
    case DEVICE_SSGL:
    case DEVICE_SSGLP:     return createCSSG(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier));
    // SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771) は YM2149 相当の SSG を
    // 2系統内蔵し、chごとのパンポットを持つ 6ch チップのため CSSG そのもの
    // ではなく派生の CSSGS / CSSGS3 を使う。SSGSとSSGS2はレジスタマップが
    // 完全に同一で違いはマスタークロックの分周比だけ、SSGS3はさらに
    // レジスタ配置とパンポット分解能が異なる (ssgsSsgBlockClock / CSSGS3 参照)。
    case DEVICE_SSGS:
    case DEVICE_SSGS2:
    case DEVICE_SSGS3:     return createCSSGS(
                                port,
                                ssgsSsgBlockClock(deviceType, port, sampleRate, clockDivider,
                                                  clockMultiplier),
                                deviceType);
    // Y8960 の SSG部は SSGS と同じ CSSGS 系だが、プロファイルに書くクロックは
    // SSG ブロックの動作クロックそのもの (単体の SSG と同じ意味) のため換算しない。
    case DEVICE_SSGEX:     return createCSSGS(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier),
                                              deviceType);
    case DEVICE_EPSG:      return createCEPSG(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier));

    case DEVICE_DCSG:      return createCDCSG(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier));
    // DSG (YM2163) はレジスタマップ・音色モデルともSSGとは全く別物のため
    // 専用ドライバを持つ。
    case DEVICE_DSG:       return createCDSG(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier));
    case DEVICE_DSG_RHY:   return createCDSGRhythm(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier));
    case DEVICE_SCC:
    case DEVICE_SCCP:
    case DEVICE_SCCEX:     return createCSCC(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier),
                                              deviceType);
    case DEVICE_SAA:       return createCSAA1099(port, psgMasterClock(port, sampleRate, clockDivider, clockMultiplier));

    case DEVICE_ADPCMA:
    case DEVICE_ADPCMB:
    case DEVICE_ADPCMB_OPNA:
    case DEVICE_ADPCMB_Y8950:
    case DEVICE_ADPCMB_OPL2EX:
    case DEVICE_PCMD8:
    case DEVICE_MA1:
    case DEVICE_MA2:
    case DEVICE_SSGS_ADPCM: return createCAdPcm(port, sampleRate, deviceType);
    case DEVICE_OPL4AWM:   return createCOPL4AWM(port, sampleRate);

    default:
        FITOM_LOG_WARN("DeviceFactory: unsupported device type 0x"
            << std::hex << deviceType);
        return nullptr;
    }
}

bool DeviceFactory::isSupported(uint32_t t) {
    return t != DEVICE_NONE && t != 0;
}

uint8_t DeviceFactory::defaultChCount(uint32_t t) {
    switch (t) {
    case DEVICE_OPM: case DEVICE_OPP: case DEVICE_OPZ: return 8;
    case DEVICE_OPNA: case DEVICE_OPN2: case DEVICE_OPN2C:
    case DEVICE_OPN2L: case DEVICE_OPN3: case DEVICE_2610B: return 6;
    case DEVICE_OPN: case DEVICE_OPNC:                   return 3;
    case DEVICE_OPNB:                                    return 4; // ch0/ch3無効化後の実効ch数(COPNB参照)
    case DEVICE_OPL: case DEVICE_OPL2: case DEVICE_OPL2EX:
    case DEVICE_OPLL: case DEVICE_OPLL2: case DEVICE_OPLLP: case DEVICE_OPLLX:
    case DEVICE_OPLLEX:                                    return 9;
    case DEVICE_OPLL_RHY: case DEVICE_OPLLEX_RHY:          return 5;
    case DEVICE_OPL_RHY:  case DEVICE_OPL2EX_RHY:          return 5;
    case DEVICE_OPL3: case DEVICE_OPL3_2:                 return 6;
    case DEVICE_VRC7:                                     return 6;
    case DEVICE_SSG: case DEVICE_PSG:                    return 3;
    // SSGS (YMZ705) / SSGS2 (YMZ732) / SSGS3 (YMZ771) は YM2149 相当を
    // 2系統内蔵 (3ch × 2)、ADPCM部は8ch (SSGS/SSGS2共通の DEVICE_SSGS_ADPCM。
    // SSGS3はADPCMではなくAMM部を持つが当面非対応)
    case DEVICE_SSGS: case DEVICE_SSGS2: case DEVICE_SSGS3: return 6;
    case DEVICE_SSGEX:                                   return 6;
    case DEVICE_SSGS_ADPCM:                              return 8;
    case DEVICE_EPSG:                                    return 3;
    case DEVICE_DCSG:                                     return 4;
    case DEVICE_DSG:                                      return 4;
    case DEVICE_DSG_RHY:                                  return 5; // BD/HC/SDN/HHO/HHD
    case DEVICE_SCC: case DEVICE_SCCP: case DEVICE_SCCEX: return 5;
    case DEVICE_SAA:                                     return 6;
    case DEVICE_PCMD8: case DEVICE_MA2: return 8;
    // Y8950内蔵ADPCM-Bは1チャンネル(DEVICE_ADPCMB/DEVICE_ADPCMB_OPNAと
    // 同じ、実機は同時に1音のみ再生可能)。旧汎用識別子DEVICE_ADPCMは
    // ドライバを持たないため、ここでは扱わない。
    case DEVICE_ADPCMB_Y8950:                              return 1;
    // Y8960拡張OPL2部の内蔵ADPCM-Bも同様に1ch(Y8950と同一のDelta-T方式)。
    case DEVICE_ADPCMB_OPL2EX:                             return 1;
    case DEVICE_ADPCMA:                                    return 6;
    case DEVICE_OPL4AWM:                                   return 24;
    case DEVICE_OPNA_RHY:                                  return 6;
    case DEVICE_MA1:                                      return 1;
    default:                                              return 1;
    }
}

bool DeviceFactory::acceptsFallback(uint32_t deviceType, uint8_t sourceVoicePatchType,
                                     const HwPatch& patch)
{
    switch (deviceType) {
    case DEVICE_OPN:
    case DEVICE_OPNC:
        return copnAcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPN2: case DEVICE_OPN2C: case DEVICE_OPN2L:
    case DEVICE_OPNA: case DEVICE_OPN3L: case DEVICE_OPNB:
    case DEVICE_2610B: case DEVICE_F286: case DEVICE_OPN3:
        return copn2AcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPM: case DEVICE_OPP:
        return copmAcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPZ:
        return copzAcceptsFallback(sourceVoicePatchType, patch);
    case DEVICE_OPZ2:
        return copz2AcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPL: case DEVICE_Y8950:
        return coplAcceptsFallback(sourceVoicePatchType, patch);
    // OPL2EX(Y8960拡張OPL2部)のFMコアはOPL2(YM3812)とレジスタマップが
    // 完全に同一のため、VoicePatchType共有(deviceTypeToVoicePatchType)
    // だけでなくフォールバック判定もCOPL2と同じ関数を使う。
    case DEVICE_OPL2: case DEVICE_OPL2EX:
        return copl2AcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_SSG: case DEVICE_PSG: case DEVICE_SSGL:
    case DEVICE_SSGLP: case DEVICE_SSGS: case DEVICE_SSGS2:
    case DEVICE_SSGS3: case DEVICE_SSGEX:
        return cssgAcceptsFallback(sourceVoicePatchType, patch);
    // DEVICE_DSG はフォールバック非対応。ROM固定音色しか持たず、他チップの
    // 音色パラメータを受け取っても再現できないため (OPLLのプリセット音色を
    // フォールバック対象外にしているのと同じ理由)。
    case DEVICE_EPSG:
        return cepsgAcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPLL: case DEVICE_OPLL2: case DEVICE_OPLLP:
    case DEVICE_OPLLX: case DEVICE_VRC7: case DEVICE_OPLLEX: {
        uint8_t selfVpt = FITOMConfig::deviceTypeToVoicePatchType(deviceType);
        return opllFamilyAcceptsFallback(sourceVoicePatchType, selfVpt, patch);
    }

    default:
        return false; // フォールバック非対応チップ
    }
}

} // namespace fitom
