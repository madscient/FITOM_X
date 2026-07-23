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

    // アドレス範囲はダンプの実サイズから直接組み立てる(1ポート機は
    // 0x00-0xFF、2ポート機や単一ポートで0x100超のアドレス空間を使う
    // チップ[OPNA/OPN2/OPL3等]は0x000-0x1FF等。CFITOM::getPhysicalChipRegisterDump()
    // 側のサイズ決定ロジック参照)。
    char header[176];
    std::snprintf(header, sizeof(header), "%s  [%s]  0x%03X-0x%03X",
                  info.label.c_str(), info.physicalName.c_str(),
                  0, static_cast<unsigned>(dump.size()) - 1);
    ImGui::SeparatorText(header);

    constexpr int kCols = 16;
    const int rows = static_cast<int>(dump.size() / kCols);

    char tableId[32];
    std::snprintf(tableId, sizeof(tableId), "regtable##%d", chipIndex);

    // テーブル自体はスクロールさせず、行数分の高さへ自然にフィットさせる
    // (outer_size.y=0.0fで自動サイズ、ScrollY未指定)。ウィンドウ全体の
    // 内容が窓の高さを超えた場合は、ルートウィンドウ側のスクロールに
    // 任せる(MIDIモニターバンドと同じ挙動、2026年7月修正。以前はテーブル
    // ごとに高さを見積もってScrollYを付けていたが、見積もりがセル
    // パディング分だけ実際の内容より小さくなり、バンド単位で常に
    // スクロールバーが出てしまっていた)。
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg;
    if (ImGui::BeginTable(tableId, kCols + 1, flags)) {
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
