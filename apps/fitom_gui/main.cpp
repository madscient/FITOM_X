// apps/fitom_gui/main.cpp
//
// Dear ImGuiベースのGUIアプリケーション エントリポイント。
// GLFW(ウィンドウ/入力) + OpenGL3(描画)バックエンドを使用する。
// fitom_core には直接触れず、gui/bridge (FITOMBridge) 経由でのみ
// コアにアクセスする。
//
// 使い方: fitom_gui [profile.json]
// プロファイルを省略した場合、コアは未初期化のまま画面のみ表示する
// (デバイス一覧・MIDI入力一覧は空)。

#include "FITOMBridge.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h> // システムOpenGLヘッダーも引き込む

#include <cstdio>
#include <string>
#include <filesystem>
#include <array>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <climits>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <climits>
#endif

namespace fs = std::filesystem;

namespace {

void glfwErrorCallback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// 実行ファイル自身のディレクトリを取得する (フォントファイルの相対パス
// 解決に使う。fitom_cli の同名ヘルパーと同じ実装)。取得できなければ
// カレントディレクトリを返す。
fs::path exeDir()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return fs::path(buf).parent_path();
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#elif defined(__APPLE__)
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::path(buf).parent_path();
#endif
    return fs::current_path();
}

// 日本語グリフを含むフォントを読み込む。ImGuiのデフォルトフォントは
// ASCII相当のみで、日本語文字列を描画すると全て「?」になってしまう
// ため、apps/fitom_gui/assets/fonts/ に同梱したNoto Sans JPを読み込む。
// 見つからない場合は警告を出し、デフォルトフォントのまま続行する
// (日本語部分は文字化けするが、起動自体は継続できるようにする)。
void loadJapaneseFont(ImGuiIO& io)
{
    // 実行ファイルからの相対位置(インストール後のレイアウト)と、
    // ビルドディレクトリから見たソースツリー上の位置の両方を試す
    // (開発中、fitom_gui をビルドディレクトリから直接実行するケースに
    // 対応するため)。
    const fs::path candidates[] = {
        exeDir() / "assets" / "fonts" / "NotoSansJP-Regular.ttf",
        exeDir() / ".." / ".." / "apps" / "fitom_gui" / "assets" / "fonts" / "NotoSansJP-Regular.ttf",
    };

    for (const auto& path : candidates) {
        if (!fs::exists(path)) continue;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = true;
        ImFont* font = io.Fonts->AddFontFromFileTTF(
            path.string().c_str(), 18.0f, &cfg, io.Fonts->GetGlyphRangesJapanese());
        if (font) {
            std::fprintf(stderr, "Loaded Japanese font: %s\n", path.string().c_str());
            return;
        }
    }
    std::fprintf(stderr,
        "Warning: Japanese font not found (apps/fitom_gui/assets/fonts/NotoSansJP-Regular.ttf). "
        "Japanese text will not render correctly.\n");
}

// FITOMBridge::getDevices/getMidiInputs の内容をそれぞれ描画する。
[[maybe_unused]] void renderDeviceList(const FITOMBridge& bridge)
{
    if (!ImGui::CollapsingHeader("デバイス一覧", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto devices = bridge.getDevices();
    if (devices.empty()) {
        ImGui::TextDisabled("(デバイスがありません)");
        return;
    }
    if (ImGui::BeginTable("devices", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("ラベル");
        ImGui::TableSetupColumn("種別");
        ImGui::TableSetupColumn("ch数");
        ImGui::TableHeadersRow();
        for (const auto& dev : devices) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", dev.index);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(dev.label.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(dev.descriptor.c_str());
            ImGui::TableSetColumnIndex(3); ImGui::Text("%d", dev.chCount);
        }
        ImGui::EndTable();
    }
}

[[maybe_unused]] void renderMidiInputList(const FITOMBridge& bridge)
{
    if (!ImGui::CollapsingHeader("MIDI入力一覧", ImGuiTreeNodeFlags_DefaultOpen)) return;
    auto inputs = bridge.getMidiInputs();
    if (inputs.empty()) {
        ImGui::TextDisabled("(MIDI入力がありません)");
        return;
    }
    for (const auto& m : inputs) {
        ImGui::BulletText("[%d] %s", m.index, m.name.c_str());
    }
}

[[maybe_unused]] void renderMasterControls(FITOMBridge& bridge)
{
    if (!ImGui::CollapsingHeader("マスターコントロール", ImGuiTreeNodeFlags_DefaultOpen)) return;

    int vol = bridge.getMasterVolume();
    if (ImGui::SliderInt("マスターボリューム", &vol, 0, 127)) {
        bridge.setMasterVolume(static_cast<uint8_t>(vol));
    }

    double pitch = bridge.getMasterPitch();
    if (ImGui::InputDouble("マスターピッチ(Hz)", &pitch, 1.0, 10.0, "%.2f")) {
        bridge.setMasterPitch(pitch);
    }

    if (ImGui::Button("オールノートオフ")) bridge.allNoteOff();
    ImGui::SameLine();
    if (ImGui::Button("リセットオールコントローラー")) bridge.resetAllCtrl();
}

// ─── MIDIモニター(試作): CH1のみ、列構成の確認用 ────────────────────
// ImGui::Tableではなく固定X座標で列を並べる方式にしている。理由:
// キーボードビュー行(次段階で実装)がテーブル幅いっぱいに独自描画
// (ImDrawList)する必要があり、ImGui::Tableの自動列管理とは相性が
// 悪いため。列幅はここで確定させ、以後全チャンネル分に展開する。
// 与えられた文字列を、maxWidth(ピクセル)に収まるよう末尾を"..."で
// 省略して描画する。全体がホバーされたときは、省略前の全文をツール
// チップで表示する。
void textEllipsis(const std::string& text, float maxWidth)
{
    const ImVec2 fullSize = ImGui::CalcTextSize(text.c_str());
    if (fullSize.x <= maxWidth) {
        ImGui::TextUnformatted(text.c_str());
        return;
    }

    const float ellipsisWidth = ImGui::CalcTextSize("...").x;
    const float avail = maxWidth - ellipsisWidth;

    // 収まる文字数を先頭から1文字ずつ増やして探す(バイト単位ではなく
    // UTF-8の文字境界を壊さないよう、ImGuiのCalcTextSize自体は
    // バイト列に対して幅を返すため、マルチバイト文字の途中で切らない
    // ようにUTF-8のリード文字境界でのみ区切る)。
    size_t cut = 0;
    for (size_t i = 0; i <= text.size(); ) {
        // 次のUTF-8文字境界へ進める
        size_t next = i;
        if (next < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[next]);
            size_t len = (c < 0x80) ? 1 : (c >> 5 == 0x6) ? 2 : (c >> 4 == 0xE) ? 3 : (c >> 3 == 0x1E) ? 4 : 1;
            next += len;
        } else {
            next = text.size() + 1; // ループ終了用
        }
        if (next > text.size()) break;

        float w = ImGui::CalcTextSize(text.c_str(), text.c_str() + next).x;
        if (w > avail) break;
        cut = next;
        i = next;
    }

    std::string truncated = text.substr(0, cut) + "...";
    ImGui::TextUnformatted(truncated.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text.c_str());
    }
}

struct MonitorColumns {
    static constexpr float ch       = 0.0f;
    static constexpr float bank     = 34.0f;
    static constexpr float program  = 254.0f;
    static constexpr float volume   = 474.0f;
    static constexpr float note     = 544.0f;
    static constexpr float device   = 604.0f;
    static constexpr float fnumber  = 694.0f;
    static constexpr float total    = 880.0f; // キーボードビュー行の幅

    // 各列の表示可能幅(次の列の開始位置との差分から、余白分を引く)
    static constexpr float bankWidth    = program - bank   - 8.0f;
    static constexpr float programWidth = volume  - program - 8.0f;
};

void renderMonitorHeader()
{
    using C = MonitorColumns;
    ImGui::SetCursorPosX(C::ch);      ImGui::TextUnformatted("CH");
    ImGui::SameLine(C::bank);         ImGui::TextUnformatted("Bank");
    ImGui::SameLine(C::program);      ImGui::TextUnformatted("Program");
    ImGui::SameLine(C::volume);       ImGui::TextUnformatted("Volume");
    ImGui::SameLine(C::note);         ImGui::TextUnformatted("Note");
    ImGui::SameLine(C::device);       ImGui::TextUnformatted("Device");
    ImGui::SameLine(C::fnumber);      ImGui::TextUnformatted("Fnumber");
}

// 1チャンネル分のデータ行(バンク/プログラム/ボリューム/ノート/デバイス/
// Fnumber)を描画する。発音していない間はNote以降を空欄にする。
void renderMonitorDataRow(const FITOMChannelMonitor& mon)
{
    using C = MonitorColumns;
    char buf[64];

    ImGui::SetCursorPosX(C::ch);
    ImGui::Text("%d", mon.ch + 1);

    ImGui::SameLine(C::bank);
    if (!mon.bankName.empty()) {
        std::snprintf(buf, sizeof(buf), "%d:%d %s", mon.bankNo, mon.progNo, mon.bankName.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%d:%d <Bank name>", mon.bankNo, mon.progNo);
    }
    textEllipsis(buf, C::bankWidth);

    ImGui::SameLine(C::program);
    if (!mon.progName.empty()) {
        std::snprintf(buf, sizeof(buf), "%d: %s", mon.progNo, mon.progName.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "%d: <Prog name>", mon.progNo);
    }
    textEllipsis(buf, C::programWidth);

    ImGui::SameLine(C::volume);
    ImGui::Text("%d", mon.volume);

    // Note以降: 発音中のみ表示する。
    ImGui::SameLine(C::note);
    if (mon.sounding && mon.lastNote != 0xFF) {
        ImGui::TextUnformatted(mon.noteName.c_str());
    }

    ImGui::SameLine(C::device);
    if (mon.sounding && !mon.deviceName.empty()) {
        ImGui::Text("%d %s", mon.deviceIndex, mon.deviceName.c_str());
    }

    ImGui::SameLine(C::fnumber);
    if (mon.sounding) {
        ImGui::Text("%d:%d", mon.fnumBlock, mon.fnum);
    }
}

// ─── キーボードビュー(128ノート)+発光エフェクト ────────────────────
// 実際の鍵盤の形(白鍵/黒鍵の幅の違い等)は再現せず、128ノートを均等幅の
// 列として並べ、黒鍵に相当する列は背が低い/暗い色にすることで簡易的に
// 鍵盤らしく見せる(このビューは演奏用ではなくモニター用のため)。
struct KeyGlowState {
    uint8_t note      = 0xFF; // 0xFF=現在発光対象なし
    float   brightness = 0.0f; // 0-1、ノートオン時のベロシティ由来
    float   releasedAt = -1.0f; // 発音が止まった時刻(ImGui::GetTime()基準)。
                                 // -1の間はまだ発音中(フェード開始前)。
};

bool isBlackKeyNote(int note)
{
    int p = note % 12;
    return p == 1 || p == 3 || p == 6 || p == 8 || p == 10;
}

void renderKeyboardView(const FITOMChannelMonitor& mon, KeyGlowState& glow, float now)
{
    using C = MonitorColumns;
    constexpr float kHeight    = 20.0f;
    constexpr int   kNumNotes  = 128;
    constexpr float kFadeSec   = 0.25f; // ノートオフ後、発光が消えるまでの時間

    // 発音中なら常に最新の状態(ノート・ベロシティ)に更新し、
    // フェード状態(releasedAt)をリセットする。発音が止まった瞬間
    // (sounding: true→false)を検出したら、その時刻を記録してフェード
    // を開始する。
    if (mon.sounding && mon.lastNote != 0xFF) {
        glow.note       = mon.lastNote;
        glow.brightness = static_cast<float>(mon.velocity) / 127.0f;
        glow.releasedAt = -1.0f;
    } else if (glow.note != 0xFF && glow.releasedAt < 0.0f) {
        glow.releasedAt = now;
    }

    float glowAlpha = 0.0f;
    if (glow.note != 0xFF) {
        if (glow.releasedAt < 0.0f) {
            glowAlpha = glow.brightness;
        } else {
            float t = (now - glow.releasedAt) / kFadeSec;
            if (t >= 1.0f) {
                glow.note = 0xFF; // フェード完了。以後は発光無し。
            } else {
                glowAlpha = glow.brightness * (1.0f - t);
            }
        }
    }

    ImGui::SetCursorPosX(0.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float keyWidth = C::total / static_cast<float>(kNumNotes);

    for (int n = 0; n < kNumNotes; ++n) {
        const bool  black = isBlackKeyNote(n);
        const float x0 = origin.x + static_cast<float>(n) * keyWidth;
        const float x1 = x0 + keyWidth - 1.0f; // 1px の隙間で列を区切る
        const float y0 = origin.y;
        const float y1 = origin.y + (black ? kHeight * 0.6f : kHeight);

        ImU32 col = black ? IM_COL32(24, 24, 28, 255) : IM_COL32(220, 220, 214, 255);
        if (glow.note == n && glowAlpha > 0.001f) {
            // ベロシティ(明るさ)に応じて赤→オレンジ→黄色寄りに変化させる。
            const int g = static_cast<int>(120 + 135 * glowAlpha);
            const int b = static_cast<int>(40  * (1.0f - glowAlpha));
            col = IM_COL32(255, g, b, 255);
        }
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
    }

    ImGui::Dummy(ImVec2(C::total, kHeight));
}

// MIDIモニター バンド。ルート画面に常時表示する主要コンテンツ。
// MPU(16chの処理単位、現状最大4面)を`<`/`>`ボタンで切り替えられる。
void renderMidiMonitorBand(FITOMBridge& bridge)
{
    using C = MonitorColumns;

    static int mpuIndex = 0;
    static std::array<KeyGlowState, 16> keyGlow{};

    int mpuCount = bridge.getMpuCount();
    if (mpuCount <= 0) mpuCount = 1;
    if (mpuIndex >= mpuCount) mpuIndex = 0;

    // ポート名ラベルと切替ボタン(右上)。
    auto midiInputs = bridge.getMidiInputs();
    std::string portName;
    for (const auto& m : midiInputs) {
        if (m.index == mpuIndex) { portName = m.name; break; }
    }
    if (!portName.empty()) {
        ImGui::Text("<%s>", portName.c_str());
    } else {
        ImGui::TextDisabled("<MIDI port name>");
    }
    ImGui::SameLine(C::total - 44.0f);
    ImGui::BeginDisabled(mpuCount <= 1);
    if (ImGui::ArrowButton("##mpu_prev", ImGuiDir_Left)) {
        mpuIndex = (mpuIndex - 1 + mpuCount) % mpuCount;
        keyGlow.fill(KeyGlowState{}); // 別ポートの残像を持ち越さない
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##mpu_next", ImGuiDir_Right)) {
        mpuIndex = (mpuIndex + 1) % mpuCount;
        keyGlow.fill(KeyGlowState{});
    }
    ImGui::EndDisabled();

    renderMonitorHeader();
    ImGui::Separator();

    auto monitors = bridge.getChannelMonitors(mpuIndex);
    const float now = static_cast<float>(ImGui::GetTime());

    if (monitors.empty()) {
        FITOMChannelMonitor placeholder;
        renderMonitorDataRow(placeholder);
        ImGui::NewLine(); // SameLine()チェーンの末尾のままだと次の描画が
                           // 同じ行に重なってしまうため、明示的に改行する
        renderKeyboardView(placeholder, keyGlow[0], now);
    } else {
        for (size_t i = 0; i < monitors.size() && i < keyGlow.size(); ++i) {
            renderMonitorDataRow(monitors[i]);
            ImGui::NewLine();
            renderKeyboardView(monitors[i], keyGlow[i], now);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 720, "FITOM_X", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    loadJapaneseFont(io);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // FITOMBridge初期化。プロファイルはコマンドライン第1引数で指定する
    // (省略時はコア未初期化のまま、ウィンドウのみ表示する)。
    FITOMBridge& bridge = FITOMBridge::instance();
    const std::string profilePath = (argc >= 2) ? argv[1] : std::string();
    bool coreReady = false;
    if (!profilePath.empty()) {
        coreReady = bridge.init("", profilePath);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        // コアのタイマーコールバック(1ms相当)。フレームごとに1回呼ぶ簡易実装
        // (正確な1msキックが必要になったら別スレッド化を検討する)。
        if (coreReady) bridge.onTimer(1);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // OSウィンドウ(GLFW)いっぱいに、タイトルバー/枠/リサイズハンドル
        // 無しでImGuiのルートウィンドウを敷き詰める。単一画面のアプリ
        // なので、GLFWのウィンドウ枠と別にImGui側の入れ子ウィンドウ枠
        // (いわゆるMDI風の二重フレーム)を表示する必要が無いため。
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        constexpr ImGuiWindowFlags rootFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

        ImGui::Begin("FITOM_X_Root", nullptr, rootFlags);
        if (!coreReady) {
            if (profilePath.empty()) {
                ImGui::TextUnformatted(
                    "プロファイル未指定です。コマンドライン引数で指定してください:");
                ImGui::TextUnformatted("  fitom_gui <profile.json>");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                    "初期化に失敗しました: %s", profilePath.c_str());
            }
        } else {
            // ルート画面はMIDIモニターのバンドのみを表示する。
            // デバイス一覧・MIDI入力一覧・マスターコントロール等、他の
            // ビューへの導線は今後実装する(renderDeviceList/
            // renderMidiInputList/renderMasterControlsは[[maybe_unused]]
            // として温存済み)。
            renderMidiMonitorBand(bridge);
        }
        ImGui::End();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.10f, 0.10f, 0.12f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (coreReady) bridge.exit();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
