// apps/fitom_gui/Sf2PatchPickerDialog.cpp

#include "Sf2PatchPickerDialog.h"

#include <imgui.h>
#include <cstdio>

void Sf2PatchPickerDialog::open(const Sf2PatchSelection& current)
{
    openPending_ = true;
    level_       = Level::Program; // 要件通り、初期状態はProgram階層
    bank_        = current.bank;
    selectedProg_ = current.prog;
}

void Sf2PatchPickerDialog::renderBankLevel(FITOMBridge& bridge)
{
    ImGui::TextUnformatted("SF2バンク(CC#32)を選択してください:");
    ImGui::BeginChild("##sf2ppd_bank", ImVec2(420.0f, 280.0f), true);
    const std::vector<FITOMBankInfo> banks = bridge.getSf2BankList();
    if (banks.empty()) {
        ImGui::TextDisabled("(banks.sf2_banksが設定されていません)");
    }
    for (const auto& b : banks) {
        char buf[160];
        if (!b.name.empty()) {
            std::snprintf(buf, sizeof(buf), "%d: %s", b.bankNo, b.name.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%d: <Bank name>", b.bankNo);
        }
        if (ImGui::Selectable(buf, bank_ == b.bankNo)) {
            bank_         = b.bankNo;
            selectedProg_ = -1;
            level_        = Level::Program;
        }
    }
    ImGui::EndChild();
}

void Sf2PatchPickerDialog::renderProgramLevel(FITOMBridge& bridge)
{
    ImGui::Text("プログラムを選択してください: SF2バンク%d", bank_);
    ImGui::BeginChild("##sf2ppd_program", ImVec2(420.0f, 280.0f), true);
    const std::vector<FITOMPatchInfo> patches = bridge.getSf2BankPatches(bank_);
    if (patches.empty()) {
        ImGui::TextDisabled("(このバンクにはphdrから解決できるプリセットがありません)");
    }
    for (const auto& p : patches) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%d: %s", p.prog, p.name.c_str());
        if (ImGui::Selectable(buf, selectedProg_ == p.prog, ImGuiSelectableFlags_AllowDoubleClick)) {
            selectedProg_ = p.prog;
        }
    }
    ImGui::EndChild();
}

bool Sf2PatchPickerDialog::render(FITOMBridge& bridge, Sf2PatchSelection& out)
{
    if (openPending_) {
        ImGui::OpenPopup("SF2パッチ選択");
        openPending_ = false;
    }

    bool confirmed = false;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 420.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("SF2パッチ選択", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (level_ != Level::Bank) {
            if (ImGui::Button("↑ 上へ")) {
                level_        = Level::Bank;
                selectedProg_ = -1;
            }
            ImGui::Separator();
        }

        switch (level_) {
        case Level::Bank:
            renderBankLevel(bridge);
            break;
        case Level::Program:
            renderProgramLevel(bridge);
            break;
        }

        ImGui::Separator();
        ImGui::BeginDisabled(level_ != Level::Program || selectedProg_ < 0);
        if (ImGui::Button("選択", ImVec2(120.0f, 0.0f))) {
            out.bank  = bank_;
            out.prog  = selectedProg_;
            confirmed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        if (confirmed) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return confirmed;
}
