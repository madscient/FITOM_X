// apps/fitom_gui/LevelMeterPanel.cpp

#include "LevelMeterPanel.h"

#include <imgui.h>
#include <algorithm>

namespace {

constexpr int   kPerRow    = 12;    // この本数を超えたら折り返す
constexpr float kBarW      = 26.0f;
constexpr float kBarGap    = 6.0f;
constexpr float kBarH      = 50.0f;
constexpr float kLabelH    = 16.0f;
constexpr float kRowGap    = 6.0f;

// バー1本分を描画する。posはバー(トラック)左上のスクリーン座標。
void renderOneBar(ImDrawList* dl, ImVec2 pos, const FITOMLevelChannel& ch)
{
    const ImVec2 trackMin = pos;
    const ImVec2 trackMax(pos.x + kBarW, pos.y + kBarH);

    // 恒常的に無効なチャンネル(例: OPNBのch0/ch3、OPL/OPLL系リズムモード時の
    // ch6-8)は、枠線・塗りつぶし無しの薄暗いブランクプレースホルダとして
    // 表示する(発音することが無いため、通常のバーと紛れないようにする)。
    if (!ch.enabled) {
        dl->AddRectFilled(trackMin, trackMax, IM_COL32(30, 30, 32, 255));
        const ImVec2 textSize = ImGui::CalcTextSize(ch.name.c_str());
        const ImVec2 textPos(pos.x + (kBarW - textSize.x) * 0.5f, trackMax.y + 2.0f);
        dl->AddText(textPos, IM_COL32(90, 90, 95, 255), ch.name.c_str());
        return;
    }

    dl->AddRectFilled(trackMin, trackMax, IM_COL32(45, 45, 50, 255));

    // FITOM_Xは音声合成を行わないため実際の音量信号は無く、発音中か否か+
    // ベロシティによる疑似メーターである(main.cppのrenderKeyboardView()と
    // 同じ考え方)。
    const float level = ch.sounding ? (static_cast<float>(ch.velocity) / 127.0f) : 0.0f;
    if (level > 0.0f) {
        const float fillH = kBarH * level;
        const ImVec2 fillMin(trackMin.x, trackMax.y - fillH);
        dl->AddRectFilled(fillMin, trackMax, IM_COL32(90, 200, 120, 255));
    }
    dl->AddRect(trackMin, trackMax, IM_COL32(100, 100, 105, 255));

    // ラベル(バー下部、中央揃え)。長い名前ははみ出す前提で許容する
    // (チャンネル名は数文字程度の短い接頭辞+番号のため通常問題ない)。
    const ImVec2 textSize = ImGui::CalcTextSize(ch.name.c_str());
    const ImVec2 textPos(pos.x + (kBarW - textSize.x) * 0.5f, trackMax.y + 2.0f);
    dl->AddText(textPos, IM_COL32(200, 200, 200, 255), ch.name.c_str());
}

} // namespace

void LevelMeterPanel::render(FITOMBridge& bridge)
{
    if (ImGui::SmallButton(showLogical_ ? "論理" : "物理")) {
        showLogical_ = !showLogical_;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(showLogical_ ? "(論理チップ単位)" : "(物理チップ単位)");
    ImGui::Separator();

    auto bands = showLogical_ ? bridge.getLogicalLevelBands() : bridge.getPhysicalLevelBands();
    if (bands.empty()) {
        ImGui::TextDisabled("物理チップが接続されていません");
        return;
    }

    for (const auto& band : bands) {
        renderBand(band);
    }
}

void LevelMeterPanel::renderBand(const FITOMLevelBand& band)
{
    if (band.channels.empty()) return;

    ImGui::SeparatorText(band.label.c_str());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const int rows = static_cast<int>((band.channels.size() + kPerRow - 1) / kPerRow);
    const float rowStride = kBarH + kLabelH + kRowGap;

    for (size_t i = 0; i < band.channels.size(); ++i) {
        const int row = static_cast<int>(i / kPerRow);
        const int col = static_cast<int>(i % kPerRow);
        const ImVec2 pos(origin.x + col * (kBarW + kBarGap), origin.y + row * rowStride);
        renderOneBar(dl, pos, band.channels[i]);
    }

    const int cols = std::min<int>(static_cast<int>(band.channels.size()), kPerRow);
    const ImVec2 usedSize(cols * (kBarW + kBarGap), rows * rowStride);
    // ImDrawListへ直接描画した分だけカーソルを進める(次のSeparatorText等が
    // 重ならないようにするため)。
    ImGui::Dummy(usedSize);
}
