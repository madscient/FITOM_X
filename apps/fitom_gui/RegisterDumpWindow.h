// apps/fitom_gui/RegisterDumpWindow.h
//
// レジスタダンプモニター。接続されている物理チップごとに、現在のレジスタ値
// (16進数生値)をグリッド表示する。MIDIモニターバンドとルート画面上で
// オルタネート表示(排他的に切り替え、main.cppのrenderMidiMonitorBand()の
// REG/MIDIボタン参照)されるため、このクラス自身は独立したImGuiウィンドウを
// 持たず、呼び出し側が用意した現在のウィンドウにそのまま内容を描画する。
// 値が変化したセルは背景が一瞬発光し、renderKeyboardView()の発音グロー
// (main.cpp、ベロシティ連動の発光エフェクト)と同じ「発光開始時刻からの
// 経過時間でフェード」方式で徐々に消える。表示専用(値の編集はできない)。
//
// データ源: FITOMBridge::getHwChips()/getHwChipRegisterDump()。実チップに
// レジスタ読み出しAPIは無いため、あくまでFITOM_Xが最後に書き込んだ値を表示
// する(詳細はCFITOM::getPhysicalChipRegisterDump()の宣言コメント参照)。

#pragma once

#include "FITOMBridge.h"
#include <cstdint>
#include <vector>

class RegisterDumpWindow {
public:
    // 毎フレーム、表示中の間だけ呼ぶ(可視状態の管理は呼び出し側の責務)。
    void render(FITOMBridge& bridge);

private:
    // 物理チップ1個分の表示状態(FITOMChipInfo::indexをそのままインデックス
    // として使う。物理チップ構成はセッション中固定のため、増分resizeのみで足りる)。
    struct ChipState {
        std::vector<uint8_t> lastValues;     // 直前フレームのダンプ(変化検出用)
        std::vector<float>   glowStartedAt;  // 各セルが最後に変化した時刻
                                              // (ImGui::GetTime()基準)。-1=無発光
    };
    std::vector<ChipState> chipStates_;

    void renderChipTable(int chipIndex, const FITOMChipInfo& info,
                          const std::vector<uint8_t>& dump, ChipState& state, float now);
};
