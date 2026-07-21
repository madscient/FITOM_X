// apps/fitom_gui/PatchPickerDialog.cpp

#include "PatchPickerDialog.h"

#include <imgui.h>
#include <cstdio>

namespace {

// CC#0(バンクセレクトMSB)のカテゴリ一覧。値の意味は仕様上固定のため、
// コアから動的取得する必要はない
// (docs/manuals/midi-message-reference.md 2.2節「直接デバイス選択値一覧」と同じ)。
// チャンネル役割切替値(120/121)はここに含めない
// (ChSettingsDialogの「リズム/インストゥルメント切り替え」チェックボックスの担当)。
struct CategoryEntry {
    uint8_t     value;
    const char* label;
};

constexpr CategoryEntry kCategories[] = {
    { 0x00, "通常モード" },
    { 0x10, "OPN" },
    { 0x11, "OPN2" },
    { 0x19, "OPM" },
    { 0x1A, "OPZ" },
    { 0x1B, "OPZ2" },
    { 0x20, "OPL" },
    { 0x21, "OPL2" },
    { 0x22, "OPL3(2オペレータモード)" },
    { 0x23, "OPL内蔵リズム" },
    { 0x28, "OPLL" },
    { 0x29, "OPLLP" },
    { 0x2A, "OPLLX" },
    { 0x2B, "VRC7" },
    { 0x30, "OPL3(4オペレータモード)" },
    { 0x40, "SSG" },
    { 0x41, "EPSG" },
    { 0x42, "DCSG" },
    { 0x43, "SAA" },
    { 0x48, "SCC" },
    { 0x51, "ADPCM-B" },
    { 0x52, "ADPCM-A" },
    { 0x53, "PCM-D8" },
    { 0x54, "AWM" },
};

const char* categoryLabel(uint8_t value)
{
    for (const auto& c : kCategories) {
        if (c.value == value) return c.label;
    }
    return "(不明なカテゴリ)";
}

// 試聴用の固定ノート(C4)とベロシティ。
constexpr uint8_t kAuditionNote     = 60;
constexpr uint8_t kAuditionVelocity = 100;

} // namespace

void PatchPickerDialog::open(int mpuIndex, int ch, const PatchSelection& current)
{
    mpuIndex_         = mpuIndex;
    ch_               = ch;
    openedWith_       = current;
    auditionSounding_ = false;

    openPending_  = true;
    level_        = Level::Program; // 要件通り、初期状態はProg.chg階層
    category_     = current.voicePatchType;
    bank_         = current.bankNo;
    selectedProg_ = current.progNo;
}

void PatchPickerDialog::auditionSelect(FITOMBridge& bridge, uint8_t voicePatchType, int bankNo, int progNo)
{
    stopAudition(bridge); // 前の試聴ノートが鳴っていれば止めてから切り替える
    bridge.sendControlChange(mpuIndex_, ch_, 0, voicePatchType);
    bridge.sendControlChange(mpuIndex_, ch_, 32, static_cast<uint8_t>(bankNo));
    bridge.sendProgramChange(mpuIndex_, ch_, static_cast<uint8_t>(progNo));
    bridge.sendNoteOn(mpuIndex_, ch_, kAuditionNote, kAuditionVelocity);
    auditionSounding_ = true;
}

void PatchPickerDialog::stopAudition(FITOMBridge& bridge)
{
    if (!auditionSounding_) return;
    bridge.sendNoteOff(mpuIndex_, ch_, kAuditionNote);
    auditionSounding_ = false;
}

void PatchPickerDialog::restoreOriginal(FITOMBridge& bridge)
{
    stopAudition(bridge);
    bridge.sendControlChange(mpuIndex_, ch_, 0, openedWith_.voicePatchType);
    bridge.sendControlChange(mpuIndex_, ch_, 32, static_cast<uint8_t>(openedWith_.bankNo));
    bridge.sendProgramChange(mpuIndex_, ch_, static_cast<uint8_t>(openedWith_.progNo));
}

void PatchPickerDialog::renderCategoryLevel()
{
    ImGui::TextUnformatted("カテゴリ(CC#0)を選択してください:");
    ImGui::BeginChild("##ppd_category", ImVec2(420.0f, 280.0f), true);
    for (const auto& c : kCategories) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%3d (0x%02X)  %s", c.value, c.value, c.label);
        if (ImGui::Selectable(buf, category_ == c.value)) {
            category_     = c.value;
            bank_         = 0;
            selectedProg_ = -1;
            level_        = Level::Bank;
        }
    }
    ImGui::EndChild();
}

void PatchPickerDialog::renderBankLevel(FITOMBridge& bridge)
{
    ImGui::Text("バンク(CC#32)を選択してください: [%s]", categoryLabel(category_));
    ImGui::BeginChild("##ppd_bank", ImVec2(420.0f, 280.0f), true);
    const std::vector<FITOMBankInfo> banks = (category_ == 0)
        ? bridge.getPatchBankList()
        : bridge.getHwBankList(category_);
    if (banks.empty()) {
        ImGui::TextDisabled("(登録されているバンクがありません)");
    }
    for (const auto& b : banks) {
        char buf[128];
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

void PatchPickerDialog::renderProgramLevel(FITOMBridge& bridge)
{
    ImGui::Text("プログラムを選択してください: [%s] バンク%d", categoryLabel(category_), bank_);
    ImGui::BeginChild("##ppd_program", ImVec2(420.0f, 280.0f), true);
    const std::vector<FITOMPatchInfo> patches = (category_ == 0)
        ? bridge.getPatches(bank_)
        : bridge.getHwBankPatches(category_, bank_);
    if (patches.empty()) {
        ImGui::TextDisabled("(登録されているパッチがありません)");
    }
    for (const auto& p : patches) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%d: %s", p.prog, p.name.c_str());
        const std::string selId = std::string(buf) + "##ppd_prog" + std::to_string(p.prog);
        ImGui::Selectable(selId.c_str(), selectedProg_ == p.prog);
        // マウスボタンダウンでNote On、アップでNote Offにする(鍵盤を
        // 押している間だけ鳴る動作、2026年7月)。IsItemActivated()/
        // IsItemDeactivated()は、Selectable()の戻り値(クリック確定、
        // 既定ではボタンアップ時にtrueになる)とは別に、ImGui内部の
        // ActiveId(マウスダウンで設定されアップされるまで保持される)の
        // 変化を直接見て判定するため、押下と離しをそれぞれ検出できる。
        // (以前はダブルクリックでの即時確定も兼ねていたが、連打すると
        // 試聴のNote Onと確定側のstopAudition()が競合し音が鳴らなくなる
        // 不具合があったため、確定は下の「選択」ボタンに一本化済み)。
        if (ImGui::IsItemActivated()) {
            selectedProg_ = p.prog;
            auditionSelect(bridge, category_, bank_, selectedProg_);
        }
        if (ImGui::IsItemDeactivated()) {
            stopAudition(bridge);
        }
    }
    ImGui::EndChild();
}

bool PatchPickerDialog::render(FITOMBridge& bridge, PatchSelection& out)
{
    if (openPending_) {
        ImGui::OpenPopup("パッチ選択");
        openPending_ = false;
    }

    bool confirmed = false;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 420.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("パッチ選択", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (level_ != Level::Category) {
            if (ImGui::Button("↑ 上へ")) {
                stopAudition(bridge); // 一覧を離れるので試聴ノートは止める
                level_        = (level_ == Level::Program) ? Level::Bank : Level::Category;
                selectedProg_ = -1;
            }
            ImGui::Separator();
        }

        switch (level_) {
        case Level::Category:
            renderCategoryLevel();
            break;
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
            out.voicePatchType = category_;
            out.bankNo         = bank_;
            out.progNo         = selectedProg_;
            confirmed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("キャンセル", ImVec2(120.0f, 0.0f))) {
            // 試聴で変わってしまったチャンネルの状態をopen()時点へ戻す。
            restoreOriginal(bridge);
            ImGui::CloseCurrentPopup();
        }

        if (confirmed) {
            // 確定した音色は試聴中のものと同じなので、値は戻さずノートだけ止める。
            stopAudition(bridge);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return confirmed;
}
