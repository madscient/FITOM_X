// apps/fitom_gui/SystemSettingsDialog.h
//
// MIDIモニター左上の歯車アイコンボタンをクリックしたときに開く、システム
// 設定モーダルダイアログ。マスターボリューム・マスターピッチと、部位ごとの
// ゲイン(チップが別々の端子から出す出力[OPNAのFMとSSG等]のバランス。対応する
// hwifプラグインのチップがある場合のみ表示)を扱う
// (今後設定項目が増える前提のため、ChSettingsDialog/MidiPortSettingsDialog
// と同じく専用ファイルに分離している)。
//
// 適用方式: ChSettingsDialogのVolume/Panpot/Expressionスライダーと同じく、
// 操作するたびにFITOMBridgeのsetterを呼んで即時プレビューし、キャンセル時は
// 開いた時点の値へ復元する。OKで閉じたときは最終値を改めて送信したうえで、
// FITOMBridge::saveCurrentProfile()経由で現在のプロファイルファイルへ
// 書き戻す。

#pragma once

#include "FITOMBridge.h"
#include <cstdint>
#include <string>
#include <vector>

class SystemSettingsDialog {
public:
    // ダイアログを開く。現在値をbridgeから取得し、ローカルな編集状態を
    // 初期化する。
    void open(FITOMBridge& bridge);

    // 毎フレーム呼ぶ。ダイアログが開いていなければ何もしない。
    void render(FITOMBridge& bridge);

private:
    bool openPending_ = false;

    // 開いた時点のスナップショット(キャンセル時の復元用)。
    uint8_t initialVolume_ = 100;
    double  initialPitch_  = 440.0;

    // ローカル編集状態(ImGuiのスライダーAPIの型に合わせる)。
    int   volume_ = 100;
    float pitch_  = 440.0f;

    // ─── 部位ごとのゲイン ────────────────────────────────────────────────
    struct PartRow {
        std::string name;
        float gainL = 1.0f, gainR = 1.0f;         // 編集中の値
        float initialL = 1.0f, initialR = 1.0f;   // 開いた時点の値
        float defaultL = 1.0f, defaultR = 1.0f;   // hwifプラグインの既定値
        float sliderMax = 2.0f;
        bool  linked = true;                      // LとRを同じ値で動かす
    };
    struct ChipRows {
        int                  chipIndex = 0;       // FITOMBridge::getHwChips()のindex
        std::string          title;
        std::vector<PartRow> parts;
    };
    // 部位を持つチップだけを載せる。
    std::vector<ChipRows> partGainChips_;

    void renderPartGains(FITOMBridge& bridge);
    void applyAndClose(FITOMBridge& bridge);
};
