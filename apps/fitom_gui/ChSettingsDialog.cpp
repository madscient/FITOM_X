// apps/fitom_gui/ChSettingsDialog.cpp

#include "ChSettingsDialog.h"

#include <imgui.h>
#include <array>
#include <cstdio>

void ChSettingsDialog::open(FITOMBridge& bridge, int mpuIndex, int ch)
{
    mpuIndex_ = mpuIndex;
    ch_       = ch;
    initial_  = bridge.getChannelSettings(mpuIndex, ch);

    volume_     = initial_.volume;
    expression_ = initial_.expression;
    panpot_     = initial_.panpot;
    isRhythm_   = initial_.isRhythm;
    mono_       = initial_.monoMode;

    patch_.voicePatchType = initial_.bankSelMSB;
    patch_.bankNo         = static_cast<int>(initial_.bankNo);
    patch_.progNo         = initial_.progNo;
    patchChanged_         = false;
    pickerEverOpened_     = false;

    isSf2_             = initial_.isSf2Windowed;
    sf2FluidsynthChan_ = static_cast<int>(initial_.sf2FluidsynthChan);
    sf2Patch_.bank     = initial_.sf2Bank;
    sf2Patch_.prog     = initial_.sf2Prog;

    openPending_ = true;
}

std::string ChSettingsDialog::currentPatchLabel(FITOMBridge& bridge) const
{
    char buf[160];
    if (isSf2_) {
        for (const auto& p : bridge.getSf2BankPatches(sf2Patch_.bank)) {
            if (p.prog == sf2Patch_.prog) {
                std::snprintf(buf, sizeof(buf), "SF2 %d:%d %s", sf2Patch_.bank, p.prog, p.name.c_str());
                return buf;
            }
        }
        std::snprintf(buf, sizeof(buf), "SF2 %d:%d <Patch name>", sf2Patch_.bank, sf2Patch_.prog);
        return buf;
    }
    if (isRhythm_) {
        for (const auto& p : bridge.getDrumPatches()) {
            if (p.prog == patch_.progNo) {
                std::snprintf(buf, sizeof(buf), "%d: %s", p.prog, p.name.c_str());
                return buf;
            }
        }
        std::snprintf(buf, sizeof(buf), "%d: <Drum kit name>", patch_.progNo);
        return buf;
    }

    const auto patches = (patch_.voicePatchType == 0)
        ? bridge.getPatches(patch_.bankNo)
        : bridge.getHwBankPatches(patch_.voicePatchType, patch_.bankNo);
    for (const auto& p : patches) {
        if (p.prog == patch_.progNo) {
            std::snprintf(buf, sizeof(buf), "0x%02X %d:%d %s",
                patch_.voicePatchType, patch_.bankNo, p.prog, p.name.c_str());
            return buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "0x%02X %d:%d <Patch name>",
        patch_.voicePatchType, patch_.bankNo, patch_.progNo);
    return buf;
}

void ChSettingsDialog::renderDrumPicker(FITOMBridge& bridge)
{
    if (drumPickerPending_) {
        ImGui::OpenPopup("ドラムキット選択");
        drumPickerPending_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(420.0f, 420.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("ドラムキット選択", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::BeginChild("##drum_list", ImVec2(400.0f, 300.0f), true);
        for (const auto& p : bridge.getDrumPatches()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%d: %s", p.prog, p.name.c_str());
            if (ImGui::Selectable(buf, drumSelectedProg_ == p.prog, ImGuiSelectableFlags_AllowDoubleClick)) {
                drumSelectedProg_ = p.prog;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    patch_.progNo = drumSelectedProg_;
                    patchChanged_ = true;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndChild();

        ImGui::BeginDisabled(drumSelectedProg_ < 0);
        if (ImGui::Button("選択", ImVec2(120.0f, 0.0f))) {
            patch_.progNo = drumSelectedProg_;
            patchChanged_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ChSettingsDialog::applyAndClose(FITOMBridge& bridge)
{
    // 送信順序: SF2窓の割り当て/解除(変更時のみ) → リズム切替(変更時のみ、
    // SF2モードでは無関係) → パッチ選択 → Volume → Expression → Poly/Mono。
    // SF2窓の割り当て/解除・リズム切替を先に送ることで、以降のCC#32/
    // Prog.chg送信がどちらの経路(SF2直行パス/ネイティブ)・チャンネル
    // 種別に対して行われるかが確定してから送られるようにする。
    if (isSf2_ != initial_.isSf2Windowed
        || (isSf2_ && sf2FluidsynthChan_ != static_cast<int>(initial_.sf2FluidsynthChan))) {
        bridge.setSf2ChannelWindow(mpuIndex_, ch_, isSf2_ ? sf2FluidsynthChan_ : 0x7F);
    }

    if (!isSf2_ && isRhythm_ != initial_.isRhythm) {
        bridge.sendControlChange(mpuIndex_, ch_, 0, isRhythm_ ? 120 : 121);
    }

    if (patchChanged_) {
        if (isSf2_) {
            bridge.sendControlChange(mpuIndex_, ch_, 32, static_cast<uint8_t>(sf2Patch_.bank));
            bridge.sendProgramChange(mpuIndex_, ch_, static_cast<uint8_t>(sf2Patch_.prog));
        } else if (isRhythm_) {
            bridge.sendProgramChange(mpuIndex_, ch_, static_cast<uint8_t>(patch_.progNo));
        } else {
            bridge.sendControlChange(mpuIndex_, ch_, 0, patch_.voicePatchType);
            bridge.sendControlChange(mpuIndex_, ch_, 32, static_cast<uint8_t>(patch_.bankNo));
            bridge.sendProgramChange(mpuIndex_, ch_, static_cast<uint8_t>(patch_.progNo));
        }
    }

    bridge.sendControlChange(mpuIndex_, ch_, 7, static_cast<uint8_t>(volume_));
    bridge.sendControlChange(mpuIndex_, ch_, 10, static_cast<uint8_t>(panpot_));
    bridge.sendControlChange(mpuIndex_, ch_, 11, static_cast<uint8_t>(expression_));
    if (mono_) {
        bridge.sendControlChange(mpuIndex_, ch_, 126, 1);
    } else {
        bridge.sendControlChange(mpuIndex_, ch_, 127, 0);
    }

    ImGui::CloseCurrentPopup();
}

void ChSettingsDialog::render(FITOMBridge& bridge)
{
    if (openPending_) {
        ImGui::OpenPopup("CH設定");
        openPending_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("CH設定", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("MPU%d CH%d", mpuIndex_, ch_ + 1);
        ImGui::Separator();

        // SF2直行パス(2026年8月新設): リズムチャンネル切替と同様、OK確定
        // まで実際には送信しない(ライブプレビューなし)。isRhythm_とは
        // 相互排他のため、ONにした瞬間にリズム側を強制falseにし、
        // リズムチェックボックス自体も無効化する。
        const bool wasSf2 = isSf2_;
        if (ImGui::Checkbox("SF2直行パス", &isSf2_)) {
            if (isSf2_) {
                isRhythm_ = false;
                if (!wasSf2) {
                    // 初めて有効化した場合、未使用のfluidsynth chanを
                    // 自動提案する(他の(mpu,ch)が既に使っている値は除く。
                    // 自分自身が既に使っていた値[initial_]は除外しない)。
                    std::array<bool, 16> taken{};
                    for (const auto& a : bridge.getAssignedSf2Windows()) {
                        if (a.mpu == mpuIndex_ && a.ch == ch_) continue;
                        if (a.fluidsynthChan >= 0 && a.fluidsynthChan < 16) {
                            taken[static_cast<size_t>(a.fluidsynthChan)] = true;
                        }
                    }
                    sf2FluidsynthChan_ = 0;
                    for (int i = 0; i < 16; ++i) {
                        if (!taken[static_cast<size_t>(i)]) { sf2FluidsynthChan_ = i; break; }
                    }
                }
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::BeginDisabled(!isSf2_);
        ImGui::SliderInt("fluidsynth chan", &sf2FluidsynthChan_, 0, 15);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(isSf2_);
        ImGui::Checkbox("リズムチャンネル (CC#0)", &isRhythm_);
        ImGui::EndDisabled();
        // Volume/Panpot/Expressionはドラッグ中(値が変わった瞬間)に
        // 実際にCC#7/#10/#11を送り、その場で音を確認できるようにする
        // (2026年7月新設、プレビュー再生)。最終的な値はOKでも改めて
        // 送られる(applyAndClose参照)ため、ここでの送信はプレビュー専用。
        if (ImGui::SliderInt("ボリューム (CC#7)", &volume_, 0, 127)) {
            bridge.sendControlChange(mpuIndex_, ch_, 7, static_cast<uint8_t>(volume_));
        }

        ImGui::BeginDisabled(isRhythm_);
        if (ImGui::SliderInt("パンポット (CC#10)", &panpot_, 0, 127)) {
            bridge.sendControlChange(mpuIndex_, ch_, 10, static_cast<uint8_t>(panpot_));
        }
        if (ImGui::SliderInt("エクスプレッション (CC#11)", &expression_, 0, 127)) {
            bridge.sendControlChange(mpuIndex_, ch_, 11, static_cast<uint8_t>(expression_));
        }
        bool poly = !mono_;
        if (ImGui::RadioButton("ポリ (CC#127)", poly)) mono_ = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("モノ (CC#126)", mono_)) mono_ = true;
        ImGui::EndDisabled();

        ImGui::Separator();
        if (ImGui::Button("パッチ")) {
            if (isSf2_) {
                sf2Picker_.open(sf2Patch_);
            } else if (isRhythm_) {
                drumSelectedProg_ = patch_.progNo;
                drumPickerPending_ = true;
            } else {
                pickerEverOpened_ = true;
                picker_.open(mpuIndex_, ch_, patch_);
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(currentPatchLabel(bridge).c_str());

        // パッチピッカー/ドラムキット選択は、この「CH設定」の
        // Begin/EndPopup区間の内側(真の入れ子)から描画する必要がある
        // (ChSettingsDialog.hのrender()コメント参照)。
        if (isSf2_) {
            Sf2PatchSelection newSf2Sel;
            if (sf2Picker_.render(bridge, newSf2Sel)) {
                sf2Patch_     = newSf2Sel;
                patchChanged_ = true;
            }
        } else if (isRhythm_) {
            renderDrumPicker(bridge);
        } else {
            PatchSelection newSel;
            if (picker_.render(bridge, newSel)) {
                patch_        = newSel;
                patchChanged_ = true;
            }
        }

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            applyAndClose(bridge);
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            // Volume/Panpot/Expressionはスライダー操作のたびにプレビュー
            // 送信しているため、キャンセル時は元の値へ戻す。CC送信は
            // 副作用が軽い(値を書き換えて出力レベルを再計算するだけ)ため、
            // 変更の有無を問わず常に送り直す。
            bridge.sendControlChange(mpuIndex_, ch_, 7, initial_.volume);
            bridge.sendControlChange(mpuIndex_, ch_, 10, initial_.panpot);
            bridge.sendControlChange(mpuIndex_, ch_, 11, initial_.expression);

            // パッチピッカーでの試聴によってチャンネルの状態が変わって
            // いる可能性があるため、開いたことがあれば元のCC#0/CC#32/
            // Prog.chgを送り直して復元する(ピッカー自体のキャンセルでも
            // 復元されるが、ピッカーを開いたままCH設定側をキャンセルされる
            // 経路もあるため、こちらでも保険として行う)。
            if (pickerEverOpened_) {
                bridge.sendControlChange(mpuIndex_, ch_, 0, initial_.bankSelMSB);
                bridge.sendControlChange(mpuIndex_, ch_, 32, static_cast<uint8_t>(initial_.bankNo));
                bridge.sendProgramChange(mpuIndex_, ch_, initial_.progNo);
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
