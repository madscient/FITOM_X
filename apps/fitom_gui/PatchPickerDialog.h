// apps/fitom_gui/PatchPickerDialog.h
//
// パッチ選択(CC#0/CC#32/Prog.chg)専用のモーダルダイアログ。
// CC#0(カテゴリ: 通常モード / 直接デバイス選択の各チップ種別)→
// CC#32(バンク) → Prog.chg(パッチ) の3階層をブラウジングする。
// ChSettingsDialog から、パッチ変更ボタン押下時に開かれる想定。

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
    // ダイアログを開く。current: 現在選択中の値(初期状態としてこの
    // Prog.chg階層をいきなり表示する)。
    void open(const PatchSelection& current);

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

    void renderCategoryLevel();
    void renderBankLevel(FITOMBridge& bridge);
    bool renderProgramLevel(FITOMBridge& bridge, PatchSelection& out); // trueなら確定
};
