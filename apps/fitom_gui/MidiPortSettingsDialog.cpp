// apps/fitom_gui/MidiPortSettingsDialog.cpp

#include "MidiPortSettingsDialog.h"

#include <imgui.h>
#include <algorithm>

void MidiPortSettingsDialog::open(FITOMBridge& bridge)
{
    portCount_ = std::min(bridge.getMpuCount(), kMaxPorts);

    availablePorts_ = bridge.getAvailableMidiInputPorts();
    auto current = bridge.getMidiInputPortAssignments();

    for (int i = 0; i < portCount_; ++i) {
        selected_[i] = (i < static_cast<int>(current.size())) ? current[i] : std::string();
        if (!selected_[i].empty() &&
            std::find(availablePorts_.begin(), availablePorts_.end(), selected_[i]) == availablePorts_.end()) {
            availablePorts_.push_back(selected_[i]);
        }
    }

    errorPending_ = false;
    openPending_  = true;
}

void MidiPortSettingsDialog::validateAndApply(FITOMBridge& bridge)
{
    // 重複設定(複数のポートに同じMIDI INを設定)チェックのみ行う。
    // 空文字列(未設定)同士は重複とみなさない。
    for (int i = 0; i < portCount_; ++i) {
        if (selected_[i].empty()) continue;
        for (int j = i + 1; j < portCount_; ++j) {
            if (selected_[i] == selected_[j]) {
                errorMessage_ = "MIDIポート" + std::to_string(i + 1) + "とMIDIポート"
                    + std::to_string(j + 1) + "に同じMIDI入力ポート(\"" + selected_[i]
                    + "\")が設定されています。";
                errorPending_ = true;
                return;
            }
        }
    }

    std::vector<std::string> names(selected_.begin(), selected_.begin() + portCount_);
    bridge.setMidiInputPorts(names);
    bridge.saveCurrentProfile();
    ImGui::CloseCurrentPopup();
}

void MidiPortSettingsDialog::render(FITOMBridge& bridge)
{
    if (openPending_) {
        ImGui::OpenPopup("MIDIポート設定");
        openPending_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("MIDIポート設定", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        for (int i = 0; i < portCount_; ++i) {
            const std::string label = "MIDIポート" + std::to_string(i + 1);
            const char* previewValue = selected_[i].empty() ? "(未設定)" : selected_[i].c_str();
            if (ImGui::BeginCombo(label.c_str(), previewValue)) {
                if (ImGui::Selectable("(未設定)", selected_[i].empty())) {
                    selected_[i].clear();
                }
                for (const auto& name : availablePorts_) {
                    const bool isSelected = (selected_[i] == name);
                    if (ImGui::Selectable(name.c_str(), isSelected)) {
                        selected_[i] = name;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            validateAndApply(bridge);
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        // バリデーションエラー表示。「MIDIポート設定」のBegin/EndPopup
        // 区間の内側から入れ子で描画する(ChSettingsDialogのドラムキット
        // 選択ポップアップと同じ理由。ChSettingsDialog.h参照)。
        if (errorPending_) {
            ImGui::OpenPopup("MIDIポート設定エラー");
            errorPending_ = false;
        }
        if (ImGui::BeginPopupModal("MIDIポート設定エラー", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(errorMessage_.c_str());
            if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::EndPopup();
    }
}
