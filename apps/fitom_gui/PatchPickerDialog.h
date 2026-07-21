// apps/fitom_gui/PatchPickerDialog.h
//
// パッチ選択(CC#0/CC#32/Prog.chg)専用のモーダルダイアログ。
// CC#0(カテゴリ: 通常モード / 直接デバイス選択の各チップ種別)→
// CC#32(バンク) → Prog.chg(パッチ) の3階層をブラウジングする。
// ChSettingsDialog から、パッチ変更ボタン押下時に開かれる想定。
//
// 試聴(2026年7月新設): Prog.chg階層の行をマウスボタンダウンした瞬間に
// CC#0/CC#32/Prog.chg/Note On(C4)を対象チャンネルへ送り、ボタンアップで
// Note Offを送る(鍵盤を押している間だけ鳴る動作。同じ行への連打も
// 含め、押すたびに必ず送り直す)。確定は「選択」ボタンのみで行う
// (以前はダブルクリックでも確定していたが、連打時に試聴のNote Onと
// 確定側のNote Offが競合し音が鳴らなくなる不具合があったため廃止)。
// 確定時はその音色のままNote Offのみ送る(通常は既にボタンアップで
// 止まっている)。「キャンセル」で閉じた場合は、試聴によって変わって
// しまったチャンネルの状態をopen()時点の値へ戻す。

#pragma once

#include "FITOMBridge.h"
#include <cstdint>
#include <vector>

// パッチピッカーの選択結果。CInstCh::progChange()系のCC#0/CC#32/Prog.chgと
// 同じ意味 (voicePatchType=0 なら通常モード、それ以外は直接デバイス選択)。
struct PatchSelection {
    uint8_t voicePatchType = 0;
    int     bankNo         = 0;
    int     progNo         = 0;
};

class PatchPickerDialog {
public:
    // ダイアログを開く。mpuIndex/ch: 試聴メッセージの送信先。
    // current: 現在選択中の値(初期状態としてこのProg.chg階層をいきなり
    // 表示する。キャンセル時にこの値へ復元する)。
    void open(int mpuIndex, int ch, const PatchSelection& current);

    // 毎フレーム呼ぶ。ダイアログが開いていなければ何もしない。
    // OKで確定した(その)フレームのみtrueを返し、outに結果を格納する。
    bool render(FITOMBridge& bridge, PatchSelection& out);

private:
    enum class Level { Category, Bank, Program };

    bool    openPending_ = false; // ImGui::OpenPopup()をまだ呼んでいない
    Level   level_        = Level::Program;
    uint8_t category_     = 0; // 選択中のCC#0値 (0=通常モード)
    int     bank_         = 0; // 選択中のCC#32相当値
    int     selectedProg_ = -1; // Program階層で選択中(未確定)のプログラム番号

    int             mpuIndex_        = 0;
    int             ch_              = 0;
    PatchSelection  openedWith_;             // open()時点の値(キャンセル時の復元先)
    bool            auditionSounding_ = false; // 試聴ノートが鳴っている間true

    void renderCategoryLevel();
    void renderBankLevel(FITOMBridge& bridge);
    void renderProgramLevel(FITOMBridge& bridge); // 行のボタンダウン/アップで試聴、確定は「選択」ボタン

    // Prog.chg階層で行を選択した際に呼ぶ。試聴中のノートがあれば止めてから
    // CC#0/CC#32/Prog.chg/Note On(C4)を送信する。
    void auditionSelect(FITOMBridge& bridge, uint8_t voicePatchType, int bankNo, int progNo);
    // 試聴ノートが鳴っていればNote Offを送る(値は戻さない)。
    void stopAudition(FITOMBridge& bridge);
    // 試聴ノートを止め、チャンネルをopenedWith_(open()時点)の
    // CC#0/CC#32/Prog.chgへ戻す。
    void restoreOriginal(FITOMBridge& bridge);
};
