// apps/fitom_gui/LevelMeterPanel.h
//
// チャンネルレベルメーター。チップごとに、物理チャンネル1本につき1本の
// カラーLED風セグメントバー(発音中か否か+ベロシティによる疑似メーター。
// FITOM_Xは音声合成を行わないため実音量信号は存在しない)を横に並べ、
// バーの下部にチャンネル名(例: "FM1"、"SSG1"、内蔵リズム音源なら"BD"等の
// 楽器名。CFITOM::getSubDeviceChannelName()由来)を表示する。
// バーは12本を単位に折り返す。チップごとにバンドを縦に積んで表示する。
//
// バーのレベルはノートオン/オフに連動したエンベロープ(GUI側で保持する
// 疑似減衰)で駆動する。ノートオンでベロシティ由来のピークまで即座に
// 立ち上がり、以後2秒で全減衰。途中でノートオフになった場合は、その
// 時点のレベルから500msで全減衰する(main.cppのrenderKeyboardView()の
// 発光フェードと同じ考え方)。加えてノートオン時にはピークホールド
// エフェクト(ピーク位置のセグメントが1秒かけて減衰する発光)を重ねる。
// ピークホールドはノートオフで即時消滅する。
//
// 物理チップ単位(同一物理ポートを共有するサブデバイスを1バンドにまとめる)
// と論理チップ単位(devices[]の1エントリ=1バンド)を切り替えられる。
// 恒常的に無効なチャンネルは、物理チップ単位表示ではブランクプレース
// ホルダとして歯抜けの位置を残し、論理チップ単位表示では詰めて省く。
//
// MIDIモニターバンドと同じくルート画面上でオルタネート表示される
// RegisterDumpWindowの左ペインに配置される想定(main.cpp参照)。

#pragma once

#include "FITOMBridge.h"

#include <string>
#include <unordered_map>

class LevelMeterPanel {
public:
    // チャンネル1本分のレベル/ピークホールドエンベロープ。
    // ノートオンのエッジはFITOMLevelChannel::noteOnSeq(ノートオンのたびに
    // 単調増加するシーケンス番号)のフレーム間差分から検出する。sounding/
    // velocityだけでは、同一ベロシティでの再トリガー(ボイススチールに
    // よる同一チャンネル上の連続ノートオン)を取りこぼすため使わない。
    // ノートオフはsoundingのfalse遷移で検出する。
    struct ChannelEnvelope {
        float    level         = 0.0f;  // 現在のバーレベル(0-1)
        float    decayFrom     = 0.0f;  // 現在の減衰フェーズの開始時レベル
        float    decayStart    = -1.0f; // 減衰フェーズ開始時刻(ImGui::GetTime()基準)。-1=未発音
        float    decayDurSec   = 0.0f;  // 現在の減衰フェーズの所要時間
        float    peakLevel     = 0.0f;  // ピークホールドの表示位置(0-1)
        float    peakStart     = -1.0f; // ピークホールド開始時刻。-1=非表示
        bool     prevSounding  = false;
        uint32_t prevNoteOnSeq = 0;
    };

    void render(FITOMBridge& bridge);

private:
    bool showLogical_ = false;  // false=物理チップ単位(既定), true=論理チップ単位
    std::unordered_map<std::string, ChannelEnvelope> envelopes_;

    void renderBand(const FITOMLevelBand& band, float now);
    ChannelEnvelope& envelopeFor(const std::string& bandLabel, const FITOMLevelChannel& ch);
};
