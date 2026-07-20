// apps/fitom_gui/ChSettingsDialog.h
//
// MIDIモニターのCH表示部分をクリックしたときに開く、チャンネル設定
// モーダルダイアログ。Volume(CC#7)・Expression(CC#11)・リズム/
// インストゥルメント切り替え(CC#0)・ポリ/モノ切り替え(CC#126/127)・
// パッチ選択(CC#0/CC#32/Prog.chg、PatchPickerDialog/ドラムキット一覧を
// 別途開く)を扱う。今後設定項目が増える前提のため、main.cppから
// 分離した専用ファイルにしている。
//
// 適用方式: IMidiChのsetterを個別に叩くのではなく、FITOMBridgeの
// MIDI送信メソッド(sendControlChange/sendProgramChange)経由で、
// 実際のMIDI受信時と同じコア側の処理経路(MidiProcessor::processControl/
// IMidiCh::progChange)を通す。

#pragma once

#include "FITOMBridge.h"
#include "PatchPickerDialog.h"
#include <cstdint>

class ChSettingsDialog {
public:
    // ダイアログを開く。現在値をbridge.getChannelSettings()で取得し、
    // ローカルな編集状態を初期化する。
    void open(FITOMBridge& bridge, int mpuIndex, int ch);

    // 毎フレーム呼ぶ。ダイアログが開いていなければ何もしない。
    // パッチピッカー/ドラムキット選択の子ダイアログは、この関数の
    // BeginPopupModal("CH設定")/EndPopup()区間の内側から描画する
    // (真の入れ子構造にする)。Dear ImGuiのポップアップスタックは
    // OpenPopup()呼び出し時点のg.BeginPopupStack.Sizeをそのポップアップの
    // 階層(レベル)として使うため、子ポップアップのOpenPopup呼び出しを
    // 親のBegin/EndPopup区間の外側(兄弟階層)で行うと、同じレベル0を
    // 親ポップアップと奪い合う形になり、ImGui::OpenPopupEx()内の
    // ClosePopupToLevel()によって親ポップアップ自体がOpenPopupStackから
    // 消去されてしまう(結果、親モーダルが消える実害あり。2026年7月、
    // 一度この構造を兄弟階層に分離してしまい、パッチ選択でOKを押すと
    // CH設定ごと消える不具合を作り込んだため、入れ子構造に戻した)。
    void render(FITOMBridge& bridge);

private:
    bool    openPending_ = false;
    int     mpuIndex_    = 0;
    int     ch_          = 0;

    // 開いた時点のスナップショット(リズム切替の変更検出に使う)。
    FITOMChannelSettings initial_;

    // ローカル編集状態
    int             volume_     = 100;
    int             expression_ = 127;
    bool            isRhythm_   = false;
    bool            mono_       = false;
    PatchSelection  patch_;
    bool            patchChanged_ = false; // ピッカーで実際に選び直したか

    PatchPickerDialog picker_;
    bool              drumPickerPending_ = false;
    int               drumSelectedProg_  = -1;

    // 現在のpatch_が指すパッチの表示名を解決する("<bank>:<prog> <name>"形式)。
    std::string currentPatchLabel(FITOMBridge& bridge) const;
    void        renderDrumPicker(FITOMBridge& bridge);
    void        applyAndClose(FITOMBridge& bridge);
};
