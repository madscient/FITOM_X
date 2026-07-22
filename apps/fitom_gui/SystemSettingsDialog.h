// apps/fitom_gui/SystemSettingsDialog.h
//
// MIDIモニター左上の歯車アイコンボタンをクリックしたときに開く、システム
// 設定モーダルダイアログ。マスターボリューム・マスターピッチを扱う
// (今後設定項目が増える前提のため、ChSettingsDialog/MidiPortSettingsDialog
// と同じく専用ファイルに分離している)。
//
// 適用方式: ChSettingsDialogのVolume/Panpot/Expressionスライダーと同じく、
// 操作するたびにFITOMBridge::setMasterVolume()/setMasterPitch()を呼んで
// 即時プレビューし、キャンセル時は開いた時点の値へ復元する。OKで閉じた
// ときは最終値を改めて送信したうえで、FITOMBridge::saveCurrentProfile()
// 経由で現在のプロファイルファイルへ書き戻す。

#pragma once

#include "FITOMBridge.h"
#include <cstdint>

class SystemSettingsDialog {
public:
    // ダイアログを開く。現在値をbridge.getMasterVolume()/getMasterPitch()
    // で取得し、ローカルな編集状態を初期化する。
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

    void applyAndClose(FITOMBridge& bridge);
};
