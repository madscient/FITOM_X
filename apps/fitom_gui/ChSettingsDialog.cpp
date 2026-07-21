// apps/fitom_gui/ChSettingsDialog.cpp

#include "ChSettingsDialog.h"

#include <imgui.h>
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

    openPending_ = true;
}

std::string ChSettingsDialog::currentPatchLabel(FITOMBridge& bridge) const
{
    char buf[160];
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
    // 送信順序: リズム切替(変更時のみ) → パッチ選択 → Volume → Expression
    // → Poly/Mono。リズム切替を最初に送ることで、以降のバンク/プログラム
    // 送信が新しいチャンネル種別に対して行われるようにする。
    if (isRhythm_ != initial_.isRhythm) {
        bridge.sendControlChange(mpuIndex_, ch_, 0, isRhythm_ ? 120 : 121);
    }

    if (patchChanged_) {
        if (isRhythm_) {
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

        ImGui::Checkbox("リズムチャンネル (CC#0)", &isRhythm_);
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
            if (isRhythm_) {
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
        if (isRhythm_) {
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
