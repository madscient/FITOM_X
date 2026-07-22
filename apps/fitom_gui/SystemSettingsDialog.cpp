// apps/fitom_gui/SystemSettingsDialog.cpp

#include "SystemSettingsDialog.h"

#include <imgui.h>

void SystemSettingsDialog::open(FITOMBridge& bridge)
{
    initialVolume_ = bridge.getMasterVolume();
    initialPitch_  = bridge.getMasterPitch();

    volume_ = initialVolume_;
    pitch_  = static_cast<float>(initialPitch_);

    openPending_ = true;
}

void SystemSettingsDialog::applyAndClose(FITOMBridge& bridge)
{
    bridge.setMasterVolume(static_cast<uint8_t>(volume_));
    bridge.setMasterPitch(static_cast<double>(pitch_));
    bridge.saveCurrentProfile();
    ImGui::CloseCurrentPopup();
}

void SystemSettingsDialog::render(FITOMBridge& bridge)
{
    if (openPending_) {
        ImGui::OpenPopup("システム設定");
        openPending_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("システム設定", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // マスターボリューム/マスターピッチはドラッグ中(値が変わった瞬間)に
        // 実際に反映し、その場で音を確認できるようにする(ChSettingsDialogの
        // Volume/Panpot/Expressionスライダーと同じプレビュー方式)。
        if (ImGui::SliderInt("マスターボリューム", &volume_, 0, 127)) {
            bridge.setMasterVolume(static_cast<uint8_t>(volume_));
        }
        // マスターピッチはCFITOM::setMasterPitch()側で430〜450Hzにクランプ
        // されるため、スライダーの範囲もそれに合わせる。
        if (ImGui::SliderFloat("マスターピッチ (Hz)", &pitch_, 430.0f, 450.0f, "%.1f")) {
            bridge.setMasterPitch(static_cast<double>(pitch_));
        }

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            applyAndClose(bridge);
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            bridge.setMasterVolume(initialVolume_);
            bridge.setMasterPitch(initialPitch_);
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
