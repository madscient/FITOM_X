// apps/fitom_gui/LevelMeterPanel.h
//
// チャンネルレベルメーター。チップごとに、物理チャンネル1本につき1本の
// 縦バー(発音中か否か+ベロシティによる疑似メーター。FITOM_Xは音声合成を
// 行わないため実音量信号は存在しない)を横に並べ、バーの下部にチャンネル名
// (例: "FM1"、"SSG1"、CFITOM::getSubDeviceChannelPrefix()由来)を表示する。
// バーは12本を単位に折り返す。チップごとにバンドを縦に積んで表示する。
//
// 物理チップ単位(同一物理ポートを共有するサブデバイスを1バンドにまとめる)
// と論理チップ単位(devices[]の1エントリ=1バンド)を切り替えられる。
//
// MIDIモニターバンドと同じくルート画面上でオルタネート表示される
// RegisterDumpWindowの左ペインに配置される想定(main.cpp参照)。

#pragma once

#include "FITOMBridge.h"

class LevelMeterPanel {
public:
    void render(FITOMBridge& bridge);

private:
    bool showLogical_ = false;  // false=物理チップ単位(既定), true=論理チップ単位

    void renderBand(const FITOMLevelBand& band);
};
