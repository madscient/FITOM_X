// apps/fitom_gui/Sf2PatchPickerDialog.h
//
// SF2直行パス(docs/sf2-fluidsynth-integration.md参照)専用のパッチ選択
// モーダルダイアログ。banks.sf2_banks[]のバンク(CC#32相当)→プログラム
// (phdrから解決したプリセット名つき)の2階層をブラウジングする。
// ChSettingsDialogから、「SF2直行パス」チェックボックスON時に「パッチ」
// ボタン押下で開かれる想定。
//
// PatchPickerDialogとの違い: SF2はCC#0(カテゴリ)を使わないためBank→
// Programの2階層のみで、CC#0/CC#32/Prog.chgを組み合わせた「今すぐ試聴」
// は行わない(ドラムキット選択と同じ、Selectableで選ぶだけの方式)。
// これはChSettingsDialogの「SF2直行パス」トグル自体が(リズム切替と同じく)
// OK確定まで実際には送信されないため、ピッカーを開いた時点では対象の
// (mpu,ch)がまだSF2の窓に割り当てられておらず、試聴メッセージを送っても
// 音が鳴らない(ルーティング先が無い)という制約による。

#pragma once

#include "FITOMBridge.h"
#include <cstdint>
#include <vector>

// SF2直行パスのパッチ選択結果。sf2_banks[].bank(CC#32相当)とprogの組。
struct Sf2PatchSelection {
    int bank = 0;
    int prog = 0;
};

class Sf2PatchPickerDialog {
public:
    // ダイアログを開く。current: 現在選択中の値(初期状態としてこの
    // バンクのProgram階層をいきなり表示する)。
    void open(const Sf2PatchSelection& current);

    // 毎フレーム呼ぶ。ダイアログが開いていなければ何もしない。
    // OKで確定した(その)フレームのみtrueを返し、outに結果を格納する。
    bool render(FITOMBridge& bridge, Sf2PatchSelection& out);

private:
    enum class Level { Bank, Program };

    bool  openPending_  = false;
    Level level_         = Level::Program;
    int   bank_          = 0;
    int   selectedProg_  = -1;

    void renderBankLevel(FITOMBridge& bridge);
    void renderProgramLevel(FITOMBridge& bridge);
};
