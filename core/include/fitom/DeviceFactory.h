#pragma once
// fitom/DeviceFactory.h
// IPort → ISoundDevice の生成ファクトリ
//
// FITOMConfig が生成した IPort (FmEnginePort / HWPort) を
// 対応するチップドライバ (COPN / COPM / COPLL 等) でラップして
// ISoundDevice として返す。
//
// ─── デバイス ID とチップドライバの対応 ─────────────────────────────────────
//   DEVICE_OPN / OPNA / OPN2   → COPN / COPNA / COPN2
//   DEVICE_OPM / OPP / OPZ     → COPM / COPP / COPZ
//   DEVICE_OPL / OPL2          → COPL / COPL2
//   DEVICE_OPL3                → COPL3
//   DEVICE_OPLL / OPLL2 / OPLLP→ COPLL / COPLL2 / COPLLP
//   DEVICE_SSG / PSG / EPSG    → CSSG
//   DEVICE_DCSG                → CDCSG
//   DEVICE_SCC / SCCP          → CSCC

#include "fitom/ISoundDevice.h"
#include "fitom/FITOMdefine.h"
#include <memory>
#include <string>
#include <cstdint>

namespace fitom {

class DeviceFactory {
public:
    // deviceType: FITOMdefine.h の DEVICE_* 定数
    // port: FmEnginePort または HWPort
    // sampleRate: エミュレーターバックエンド使用時のサンプルレート
    // extraPort: OPN2/OPNA のように 2 ポート必要なチップ用
    static std::unique_ptr<ISoundDevice> create(
        uint32_t deviceType,
        IPort*   port,
        int      sampleRate = 44100,
        IPort*   extraPort  = nullptr);

    // デバイス ID がチップドライバでサポートされているか
    static bool isSupported(uint32_t deviceType);

    // デバイス ID → デフォルトチャンネル数
    static uint8_t defaultChCount(uint32_t deviceType);
};

} // namespace fitom
