// apps/fitom_gui/MidiPortSettingsDialog.h
//
// MIDIモニターのMIDIポート名部分をクリックしたときに開く、MIDIポート設定
// モーダルダイアログ。MPU(getMpuCount()、現状4)分のMIDI入力ポート割り当てを
// それぞれドロップダウンで選択する。OKで閉じると、既存の全ポートを一旦
// 閉じたうえで選択内容のポートを開き直し(即時反映)、FITOMBridge経由で
// Config側の設定(現在のプロファイル状態)を更新したうえで、現在ロード中の
// プロファイルファイルへ書き戻す(FITOMBridge::saveCurrentProfile())。
//
// バリデーションは「複数のポートに同じMIDI IN名を設定していないか」の
// 重複チェックのみ行う。違反時はOKを継続させず、エラーメッセージボックス
// (モーダル)を表示する(ChSettingsDialogのドラムキット選択等と同じく、
// 親ポップアップのBegin/EndPopup区間の内側から入れ子で描画する)。

#pragma once

#include "FITOMBridge.h"
#include <array>
#include <string>
#include <vector>

class MidiPortSettingsDialog {
public:
    // ダイアログを開く。現在の割り当てと、システムが現在列挙するMIDI入力
    // ポート一覧を取得してローカル編集状態を初期化する。
    void open(FITOMBridge& bridge);

    // 毎フレーム呼ぶ。ダイアログが開いていなければ何もしない。
    void render(FITOMBridge& bridge);

private:
    // MAX_MPUS(core/include/fitom/CFITOM.h)と同数。bridge.getMpuCount()を
    // open()時点で読み、実際のポート数はそちらに合わせる。
    static constexpr int kMaxPorts = 4;

    bool openPending_ = false;
    int  portCount_   = kMaxPorts;

    // 各ポートの選択中の値("" = 未設定)。
    std::array<std::string, kMaxPorts> selected_{};

    // ドロップダウンの選択肢(システムが現在列挙するMIDI入力ポート名)。
    // 現在の割り当てが列挙結果に含まれない場合(デバイス切断中等)、
    // ダイアログを開いただけで割り当てが失われないよう、その名前も
    // 末尾に追加しておく。
    std::vector<std::string> availablePorts_;

    bool        errorPending_ = false;
    std::string errorMessage_;

    // 重複チェックを行い、問題無ければbridge.setMidiInputPorts()を呼んで
    // ポップアップを閉じる。重複があればerrorPending_を立てて何もしない
    // (呼び出し側でエラーポップアップが開かれる)。
    void validateAndApply(FITOMBridge& bridge);
};
