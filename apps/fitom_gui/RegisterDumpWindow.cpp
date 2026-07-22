// apps/fitom_gui/RegisterDumpWindow.cpp

#include "RegisterDumpWindow.h"

#include <imgui.h>
#include <cstdio>

namespace {

// レジスタ書き込みは瞬間的なイベント(ノートのように「押している間」が
// 無い)ため、ノートグロー(main.cppのkFadeSec=0.25f)より少し長めに
// フェード時間を取り、変化に気付きやすくする。
constexpr float kFadeSec = 0.6f;

ImU32 glowColor(float alpha)
{
    // 変化直後は明るい黄色でセル背景を発光させ、フェードするにつれ
    // 透明になって素のテーブル背景色に戻る。
    return IM_COL32(255, 220, 80, static_cast<int>(alpha * 200));
}

} // namespace

void RegisterDumpWindow::render(FITOMBridge& bridge)
{
    auto chips = bridge.getHwChips();
    if (chipStates_.size() < chips.size()) chipStates_.resize(chips.size());

    const float now = static_cast<float>(ImGui::GetTime());

    if (chips.empty()) {
        ImGui::TextDisabled("物理チップが接続されていません");
        return;
    }

    for (const auto& info : chips) {
        if (info.index < 0 || static_cast<size_t>(info.index) >= chipStates_.size()) continue;
        auto dump = bridge.getHwChipRegisterDump(info.index);
        renderChipTable(info.index, info, dump, chipStates_[info.index], now);
    }
}

void RegisterDumpWindow::renderChipTable(int chipIndex, const FITOMChipInfo& info,
                                          const std::vector<uint8_t>& dump,
                                          ChipState& state, float now)
{
    if (dump.empty()) return;

    // 変化検出: サイズが変わった(=初回描画)場合は「変化なし」として
    // 初期化するだけにとどめ、初回から全セルが発光する誤爆を避ける。
    if (state.lastValues.size() != dump.size()) {
        state.lastValues = dump;
        state.glowStartedAt.assign(dump.size(), -1.0f);
    } else {
        for (size_t i = 0; i < dump.size(); ++i) {
            if (dump[i] != state.lastValues[i]) {
                state.glowStartedAt[i] = now;
                state.lastValues[i] = dump[i];
            }
        }
    }

    char header[160];
    std::snprintf(header, sizeof(header), "%s  [%s]  %s",
                  info.label.c_str(), info.physicalName.c_str(),
                  info.twoPort ? "0x000-0x1FF" : "0x00-0xFF");
    ImGui::SeparatorText(header);

    constexpr int kCols = 16;
    const int rows = static_cast<int>(dump.size() / kCols);

    char tableId[32];
    std::snprintf(tableId, sizeof(tableId), "regtable##%d", chipIndex);

    // 2ポートチップは32行になりウィンドウからはみ出しうるため、
    // テーブル自体をスクロール可能にして高さをクランプする。
    float tableHeight = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(rows + 1) + 8.0f;
    if (tableHeight > 420.0f) tableHeight = 420.0f;

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable(tableId, kCols + 1, flags, ImVec2(0.0f, tableHeight))) {
        ImGui::TableSetupColumn("addr", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        for (int c = 0; c < kCols; ++c) {
            char colLabel[4];
            std::snprintf(colLabel, sizeof(colLabel), "%X", c);
            ImGui::TableSetupColumn(colLabel, ImGuiTableColumnFlags_WidthFixed, 24.0f);
        }
        ImGui::TableHeadersRow();

        for (int r = 0; r < rows; ++r) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%03X", r * kCols);

            for (int c = 0; c < kCols; ++c) {
                const size_t addr = static_cast<size_t>(r) * kCols + static_cast<size_t>(c);
                ImGui::TableSetColumnIndex(c + 1);

                float alpha = 0.0f;
                float startedAt = state.glowStartedAt[addr];
                if (startedAt >= 0.0f) {
                    float t = (now - startedAt) / kFadeSec;
                    if (t >= 1.0f) {
                        state.glowStartedAt[addr] = -1.0f;
                    } else {
                        alpha = 1.0f - t;
                    }
                }
                if (alpha > 0.001f) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, glowColor(alpha));
                }
                ImGui::Text("%02X", dump[addr]);
            }
        }
        ImGui::EndTable();
    }
}
