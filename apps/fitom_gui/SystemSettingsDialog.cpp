// apps/fitom_gui/SystemSettingsDialog.cpp

#include "SystemSettingsDialog.h"

#include <imgui.h>
#include <algorithm>

namespace {

// 1.0 = 0 dB。2.0 は約 +6 dB。
constexpr float kPartGainSliderMax = 2.0f;

} // namespace

void SystemSettingsDialog::open(FITOMBridge& bridge)
{
    initialVolume_ = bridge.getMasterVolume();
    initialPitch_  = bridge.getMasterPitch();

    volume_ = initialVolume_;
    pitch_  = static_cast<float>(initialPitch_);

    partGainChips_.clear();
    for (const auto& chip : bridge.getHwChips()) {
        auto gains = bridge.getHwChipPartGains(chip.index);
        if (gains.empty()) continue;

        ChipRows rows;
        rows.chipIndex = chip.index;
        // ラベルはプロファイルで重複しうるので、物理チップ名を併記して区別する。
        rows.title = chip.label.empty() ? chip.physicalName
                                        : chip.label + "  [" + chip.physicalName + "]";
        for (const auto& g : gains) {
            PartRow row;
            row.name     = g.name;
            row.gainL    = row.initialL = g.gainL;
            row.gainR    = row.initialR = g.gainR;
            row.defaultL = g.defaultL;
            row.defaultR = g.defaultR;
            // 既定値や保存済みの値がスライダーの上限を超えていても、触っただけで
            // 値が切り詰められないようにする。
            row.sliderMax = std::max({ kPartGainSliderMax, g.gainL, g.gainR,
                                       g.defaultL, g.defaultR });
            row.linked   = (g.gainL == g.gainR);
            rows.parts.push_back(std::move(row));
        }
        partGainChips_.push_back(std::move(rows));
    }

    openPending_ = true;
}

void SystemSettingsDialog::applyAndClose(FITOMBridge& bridge)
{
    bridge.setMasterVolume(static_cast<uint8_t>(volume_));
    bridge.setMasterPitch(static_cast<double>(pitch_));
    for (const auto& chip : partGainChips_) {
        for (const auto& part : chip.parts) {
            bridge.setHwChipPartGain(chip.chipIndex, part.name, part.gainL, part.gainR);
        }
    }
    bridge.saveCurrentProfile();
    ImGui::CloseCurrentPopup();
}

void SystemSettingsDialog::renderPartGains(FITOMBridge& bridge)
{
    if (partGainChips_.empty()) return;

    ImGui::SeparatorText("部位ゲイン (1.00 = 0 dB)");

    const float sliderWidth = 110.0f;
    // 部位名の列幅を、全チップを通して最も長い名前に揃える。
    float nameWidth = 0.0f;
    for (const auto& chip : partGainChips_) {
        for (const auto& part : chip.parts) {
            nameWidth = std::max(nameWidth, ImGui::CalcTextSize(part.name.c_str()).x);
        }
    }
    nameWidth += ImGui::GetStyle().ItemSpacing.x * 2.0f;

    for (auto& chip : partGainChips_) {
        ImGui::PushID(chip.chipIndex);
        ImGui::TextUnformatted(chip.title.c_str());
        for (auto& part : chip.parts) {
            ImGui::PushID(part.name.c_str());
            bool changed = false;

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(part.name.c_str());
            ImGui::SameLine(nameWidth);

            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderFloat("L", &part.gainL, 0.0f, part.sliderMax, "%.2f")) {
                if (part.linked) part.gainR = part.gainL;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::SliderFloat("R", &part.gainR, 0.0f, part.sliderMax, "%.2f")) {
                if (part.linked) part.gainL = part.gainR;
                changed = true;
            }
            ImGui::SameLine();
            ImGui::Checkbox("連動", &part.linked);
            ImGui::SameLine();
            if (ImGui::Button("既定値")) {
                part.gainL  = part.defaultL;
                part.gainR  = part.defaultR;
                part.linked = (part.defaultL == part.defaultR);
                changed = true;
            }

            if (changed) {
                bridge.setHwChipPartGain(chip.chipIndex, part.name, part.gainL, part.gainR);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
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

        renderPartGains(bridge);

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120.0f, 0.0f))) {
            applyAndClose(bridge);
        }
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            bridge.setMasterVolume(initialVolume_);
            bridge.setMasterPitch(initialPitch_);
            for (const auto& chip : partGainChips_) {
                for (const auto& part : chip.parts) {
                    bridge.setHwChipPartGain(chip.chipIndex, part.name,
                                             part.initialL, part.initialR);
                }
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
