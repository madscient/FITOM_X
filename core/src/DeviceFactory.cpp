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

std::unique_ptr<ISoundDevice> createCOPN(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPNA(IPort* p, int sr, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPN2(IPort* p, int sr, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPN2C(IPort* p, int sr, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPN2L(IPort* p, int sr, IPort* p2 = nullptr);
std::unique_ptr<ISoundDevice> createCOPNARhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPM(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPP(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPZ(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPL(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPL2(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPL3(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPL3_2(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPLL(IPort* p, int sr, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLL2(IPort* p, int sr, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLP(IPort* p, int sr, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLX(IPort* p, int sr, uint8_t mode);
std::unique_ptr<ISoundDevice> createCOPLLRhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCOPLRhythm(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCVRC7(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCSSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCEPSG(IPort* p, int sr);
std::unique_ptr<ISoundDevice> createCDCSG(IPort* p, int sr);
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
std::unique_ptr<ISoundDevice> DeviceFactory::create(
    uint32_t deviceType, IPort* port, int sampleRate, IPort* extraPort, bool rhythmMode)
{
    if (!port) {
        FITOM_LOG_ERR("DeviceFactory::create: port is null for device 0x"
            << std::hex << deviceType);
        return nullptr;
    }

    const uint8_t mode = rhythmMode ? 1 : 0;

    switch (deviceType) {
    case DEVICE_OPN:
    case DEVICE_OPNB:
    case DEVICE_OPNC:
        return createCOPN(port, sampleRate);

    case DEVICE_OPNA:
    case DEVICE_2610B:
    case DEVICE_F286:
    case DEVICE_OPN3:
        // extraPort が nullptr の場合 COPNA 内部で OffsetPort を生成する
        return createCOPNA(port, sampleRate, extraPort);

    case DEVICE_OPN2:
        return createCOPN2(port, sampleRate, extraPort);
    case DEVICE_OPN2C:
        return createCOPN2C(port, sampleRate, extraPort);
    case DEVICE_OPN2L:
        return createCOPN2L(port, sampleRate, extraPort);

    case DEVICE_OPNA_RHY:
        return createCOPNARhythm(port, sampleRate);

    case DEVICE_OPM:       return createCOPM(port, sampleRate);
    case DEVICE_OPP:       return createCOPP(port, sampleRate);
    case DEVICE_OPZ:
    case DEVICE_OPZ2:      return createCOPZ(port, sampleRate);

    case DEVICE_OPL:
    case DEVICE_Y8950:     return createCOPL(port, sampleRate);
    case DEVICE_OPL2:      return createCOPL2(port, sampleRate);
    case DEVICE_OPL3:
    case DEVICE_OPN3_L3:   return createCOPL3(port, sampleRate);
    case DEVICE_OPL3_2:    return createCOPL3_2(port, sampleRate);
    case DEVICE_OPL_RHY:   return createCOPLRhythm(port, sampleRate);

    case DEVICE_OPLL:      return createCOPLL(port, sampleRate, mode);
    case DEVICE_OPLL2:     return createCOPLL2(port, sampleRate, mode);
    case DEVICE_OPLLP:     return createCOPLLP(port, sampleRate, mode);
    case DEVICE_OPLLX:     return createCOPLLX(port, sampleRate, mode);
    case DEVICE_VRC7:      return createCVRC7(port, sampleRate);
    case DEVICE_OPLL_RHY:  return createCOPLLRhythm(port, sampleRate);

    case DEVICE_SSG:
    case DEVICE_PSG:
    case DEVICE_SSGL:
    case DEVICE_SSGLP:
    case DEVICE_SSGS:
    case DEVICE_DSG:       return createCSSG(port, sampleRate);
    case DEVICE_EPSG:      return createCEPSG(port, sampleRate);

    case DEVICE_DCSG:      return createCDCSG(port, sampleRate);
    case DEVICE_SCC:
    case DEVICE_SCCP:      return createCSCC(port, sampleRate, deviceType);
    case DEVICE_SAA:       return createCSAA1099(port, sampleRate);

    case DEVICE_ADPCMA:
    case DEVICE_ADPCMB:
    case DEVICE_ADPCMB_OPNA:
    case DEVICE_ADPCMB_Y8950:
    case DEVICE_PCMD8:
    case DEVICE_MA1:
    case DEVICE_MA2:       return createCAdPcm(port, sampleRate, deviceType);
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
    case DEVICE_OPN2L: case DEVICE_OPN3:                 return 6;
    case DEVICE_OPN: case DEVICE_OPNB: case DEVICE_OPNC: return 3;
    case DEVICE_OPL: case DEVICE_OPL2:
    case DEVICE_OPLL: case DEVICE_OPLL2: case DEVICE_OPLLP: case DEVICE_OPLLX: return 9;
    case DEVICE_OPLL_RHY:                                  return 5;
    case DEVICE_OPL_RHY:                                   return 5;
    case DEVICE_OPL3: case DEVICE_OPL3_2:                 return 6;
    case DEVICE_VRC7:                                     return 6;
    case DEVICE_SSG: case DEVICE_PSG:                    return 3;
    case DEVICE_EPSG:                                    return 3;
    case DEVICE_DCSG:                                     return 4;
    case DEVICE_SCC: case DEVICE_SCCP:                   return 5;
    case DEVICE_SAA:                                     return 6;
    case DEVICE_PCMD8: case DEVICE_MA2: return 8;
    // Y8950内蔵ADPCM-Bは1チャンネル(DEVICE_ADPCMB/DEVICE_ADPCMB_OPNAと
    // 同じ、実機は同時に1音のみ再生可能)。旧汎用識別子DEVICE_ADPCMは
    // 削除した(動いていなかったコードとの互換性維持は不要と判断、
    // 2026年7月)。
    case DEVICE_ADPCMB_Y8950:                              return 1;
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
    case DEVICE_OPNB:
    case DEVICE_OPNC:
        return copnAcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPN2: case DEVICE_OPN2C: case DEVICE_OPN2L:
    case DEVICE_OPNA: case DEVICE_OPN3L: case DEVICE_2610B:
    case DEVICE_F286: case DEVICE_OPN3:
        return copn2AcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPM: case DEVICE_OPP:
        return copmAcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPZ:
        return copzAcceptsFallback(sourceVoicePatchType, patch);
    case DEVICE_OPZ2:
        return copz2AcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPL: case DEVICE_Y8950:
        return coplAcceptsFallback(sourceVoicePatchType, patch);
    case DEVICE_OPL2:
        return copl2AcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_SSG: case DEVICE_PSG: case DEVICE_SSGL:
    case DEVICE_SSGLP: case DEVICE_SSGS: case DEVICE_DSG:
        return cssgAcceptsFallback(sourceVoicePatchType, patch);
    case DEVICE_EPSG:
        return cepsgAcceptsFallback(sourceVoicePatchType, patch);

    case DEVICE_OPLL: case DEVICE_OPLL2: case DEVICE_OPLLP:
    case DEVICE_OPLLX: case DEVICE_VRC7: {
        uint8_t selfVpt = FITOMConfig::deviceTypeToVoicePatchType(deviceType);
        return opllFamilyAcceptsFallback(sourceVoicePatchType, selfVpt, patch);
    }

    default:
        return false; // フォールバック非対応チップ
    }
}

} // namespace fitom
