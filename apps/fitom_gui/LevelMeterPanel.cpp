// apps/fitom_gui/LevelMeterPanel.cpp

#include "LevelMeterPanel.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int   kPerRow    = 12;    // この本数を超えたら折り返す
constexpr float kBarW      = 26.0f;
constexpr float kBarGap    = 6.0f;
constexpr float kBarH      = 72.0f;
constexpr float kLabelH    = 16.0f;
constexpr float kRowGap    = 6.0f;

// LEDセグメント分割数と、下から何本目までを緑/黄/赤ゾーンとするか
// (残りは赤ゾーン)。イメージ参照(緑が主体、上部に黄→赤)。
constexpr int   kSegments    = 14;
constexpr int   kGreenSegs   = 8;
constexpr int   kYellowSegs  = 3;
constexpr float kSegGap      = 2.0f;
// ステレオチャンネルで左右half-widthのLEDカラムを分ける中央の隙間 [px]。
constexpr float kStereoGap   = 2.0f;

constexpr float kAttackDecaySec  = 1.5f;  // ノートオン後、バーが全減衰するまでの時間
constexpr float kReleaseDecaySec = 0.25f; // ノートオフ後、バーが全減衰するまでの時間
constexpr float kPeakHoldSec     = 1.5f;  // ピークホールド発光が全減衰するまでの時間

constexpr ImU32 kColGreenLit  = IM_COL32( 80, 230, 110, 255);
constexpr ImU32 kColGreenDim  = IM_COL32( 22,  55,  30, 255);
constexpr ImU32 kColYellowLit = IM_COL32(235, 220,  60, 255);
constexpr ImU32 kColYellowDim = IM_COL32( 58,  52,  20, 255);
constexpr ImU32 kColRedLit    = IM_COL32(235,  70,  60, 255);
constexpr ImU32 kColRedDim    = IM_COL32( 58,  22,  20, 255);

ImU32 lerpColor(ImU32 a, ImU32 b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const ImVec4 fa = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 fb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        fa.x + (fb.x - fa.x) * t,
        fa.y + (fb.y - fa.y) * t,
        fa.z + (fb.z - fa.z) * t,
        fa.w + (fb.w - fa.w) * t));
}

// ノートオン/オフのエッジを検出し、エンベロープ(減衰フェーズ+ピーク
// ホールド)を進める。ノートオンはnoteOnSeq(ノートオンのたびに単調増加
// するシーケンス番号)の変化で検出する。soundingやvelocityの一致比較では、
// 同一ベロシティでの再トリガー(ボイススチールによる同一チャンネル上の
// 連続ノートオン)を取りこぼすため使わない。ノートオフはsoundingの
// false遷移で検出する(main.cppのrenderKeyboardView()の発光フェードと
// 同じ考え方)。
void updateEnvelope(LevelMeterPanel::ChannelEnvelope& env, const FITOMLevelChannel& ch, float now)
{
    const bool noteOn  = ch.sounding && (ch.noteOnSeq != env.prevNoteOnSeq);
    const bool noteOff = !ch.sounding && env.prevSounding;

    if (noteOn) {
        const float peak = static_cast<float>(ch.velocity) / 127.0f;
        env.decayFrom   = peak;
        env.decayStart  = now;
        env.decayDurSec = kAttackDecaySec;
        env.level       = peak;
        env.peakLevel   = peak;
        env.peakStart   = now;
    } else if (noteOff) {
        env.decayFrom   = env.level;
        env.decayStart  = now;
        env.decayDurSec = kReleaseDecaySec;
        env.peakLevel   = 0.0f;
        env.peakStart   = -1.0f;
    }

    if (env.decayStart >= 0.0f && env.decayDurSec > 0.0f) {
        const float t = (now - env.decayStart) / env.decayDurSec;
        env.level = (t >= 1.0f) ? 0.0f : env.decayFrom * (1.0f - t);
    }

    env.prevSounding  = ch.sounding;
    env.prevNoteOnSeq = ch.noteOnSeq;
}

// LEDカラム1本分を描画する。x範囲を指定できるため、モノラルチャンネルでは
// トラック全幅で1回、ステレオチャンネルでは左右half-widthで2回呼ぶ
// (2026年8月、L/R定位表示の追加で分離)。
void renderLedColumn(ImDrawList* dl, float xMin, float xMax, float yBottom,
                      float level, float peakLevel, float peakAlpha)
{
    level = std::clamp(level, 0.0f, 1.0f);
    const int litCount = static_cast<int>(std::lround(level * kSegments));
    const int peakSeg = (peakLevel > 0.0f && peakAlpha > 0.0f)
        ? std::clamp(static_cast<int>(std::lround(peakLevel * kSegments)) - 1, 0, kSegments - 1)
        : -1;

    const float segH = (kBarH - kSegGap * (kSegments - 1)) / kSegments;

    for (int i = 0; i < kSegments; ++i) {
        // セグメント0=最下段。下からトラック上端に向かって積む。
        const float segTop = yBottom - static_cast<float>(i + 1) * segH - static_cast<float>(i) * kSegGap;
        const ImVec2 segMin(xMin, segTop);
        const ImVec2 segMax(xMax, segTop + segH);

        ImU32 litCol, dimCol;
        if (i < kGreenSegs) {
            litCol = kColGreenLit; dimCol = kColGreenDim;
        } else if (i < kGreenSegs + kYellowSegs) {
            litCol = kColYellowLit; dimCol = kColYellowDim;
        } else {
            litCol = kColRedLit; dimCol = kColRedDim;
        }

        ImU32 col = (i < litCount) ? litCol : dimCol;
        if (i == peakSeg) {
            // ピークホールド: そのセグメントを白側へブレンドして際立たせる。
            col = lerpColor(col, IM_COL32(255, 255, 255, 255), peakAlpha * 0.85f);
        }
        dl->AddRectFilled(segMin, segMax, col);
    }
}

// バー1本分を描画する。posはバー(トラック)左上のスクリーン座標。
void renderOneBar(ImDrawList* dl, ImVec2 pos, const FITOMLevelChannel& ch,
                   const LevelMeterPanel::ChannelEnvelope& env, float now)
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

    dl->AddRectFilled(trackMin, trackMax, IM_COL32(16, 16, 18, 255));

    // カラーLED風: セグメントに分割し、下から現在レベル分だけ発光させる。
    // 未発光のセグメントも色ゾーンの暗いバリエーションで塗り、消灯中の
    // LEDらしい見た目にする(イメージ参照)。
    float peakAlpha = 0.0f;
    if (env.peakStart >= 0.0f) {
        const float t = (now - env.peakStart) / kPeakHoldSec;
        peakAlpha = (t >= 1.0f) ? 0.0f : (1.0f - t);
    }

    if (ch.stereo) {
        // ステレオ出力を持つチャンネル: トラックを左右half-widthに割り、
        // 定位係数(gainL/gainR)を掛けた高さでそれぞれ描く。エンベロープ
        // (発音イベント・減衰・ピークホールド)はL/Rで共通の1本を使い、
        // 高さだけを左右で変える(発音そのものは同じイベントのため)。
        const float mid = pos.x + kBarW * 0.5f;
        renderLedColumn(dl, trackMin.x + 1.0f, mid - kStereoGap * 0.5f, trackMax.y,
                        env.level * ch.gainL, env.peakLevel * ch.gainL, peakAlpha);
        renderLedColumn(dl, mid + kStereoGap * 0.5f, trackMax.x - 1.0f, trackMax.y,
                        env.level * ch.gainR, env.peakLevel * ch.gainR, peakAlpha);
    } else {
        renderLedColumn(dl, trackMin.x + 1.0f, trackMax.x - 1.0f, trackMax.y,
                        env.level, env.peakLevel, peakAlpha);
    }

    dl->AddRect(trackMin, trackMax, IM_COL32(70, 70, 75, 255));

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

    const float now = static_cast<float>(ImGui::GetTime());
    for (const auto& band : bands) {
        renderBand(band, now);
    }
}

LevelMeterPanel::ChannelEnvelope& LevelMeterPanel::envelopeFor(const std::string& bandLabel,
                                                                const FITOMLevelChannel& ch)
{
    // バンドラベル+チャンネル名で識別する(同名チャンネルが別バンドに
    // またがっても衝突しないように)。
    return envelopes_[bandLabel + '\x1f' + ch.name];
}

void LevelMeterPanel::renderBand(const FITOMLevelBand& band, float now)
{
    if (band.channels.empty()) return;

    // 恒常的に無効なチャンネルは、物理チップ単位表示でのみブランク
    // プレースホルダとして残す。物理表示は右ペインのレジスタダンプと
    // 物理ch番号で突き合わせて読むため歯抜けの位置に意味がある一方、
    // 論理表示は「その論理デバイスで実際に使えるch」を見るためのもので、
    // プレースホルダは冗長なだけになる(ラベルにch番号が入っているため、
    // 詰めて並べても元のch番号は追える)。
    std::vector<const FITOMLevelChannel*> visible;
    visible.reserve(band.channels.size());
    for (const auto& ch : band.channels) {
        if (showLogical_ && !ch.enabled) continue;
        visible.push_back(&ch);
    }
    if (visible.empty()) return;

    ImGui::SeparatorText(band.label.c_str());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    const int rows = static_cast<int>((visible.size() + kPerRow - 1) / kPerRow);
    const float rowStride = kBarH + kLabelH + kRowGap;

    for (size_t i = 0; i < visible.size(); ++i) {
        const FITOMLevelChannel& ch = *visible[i];
        const int row = static_cast<int>(i / kPerRow);
        const int col = static_cast<int>(i % kPerRow);
        const ImVec2 pos(origin.x + col * (kBarW + kBarGap), origin.y + row * rowStride);

        if (ch.enabled) {
            ChannelEnvelope& env = envelopeFor(band.label, ch);
            updateEnvelope(env, ch, now);
            renderOneBar(dl, pos, ch, env, now);
        } else {
            renderOneBar(dl, pos, ch, ChannelEnvelope{}, now);
        }
    }

    const int cols = std::min<int>(static_cast<int>(visible.size()), kPerRow);
    const ImVec2 usedSize(cols * (kBarW + kBarGap), rows * rowStride);
    // ImDrawListへ直接描画した分だけカーソルを進める(次のSeparatorText等が
    // 重ならないようにするため)。
    ImGui::Dummy(usedSize);
}
