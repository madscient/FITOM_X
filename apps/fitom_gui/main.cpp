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

namespace {

void glfwErrorCallback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// FITOMBridge::getDevices/getMidiInputs の内容をそれぞれ描画する。
void renderDeviceList(const FITOMBridge& bridge)
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

void renderMidiInputList(const FITOMBridge& bridge)
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

void renderMasterControls(FITOMBridge& bridge)
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

        ImGui::Begin("FITOM_X");
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
            ImGui::Text("プロファイル: %s", bridge.currentProfilePath().c_str());
            ImGui::Separator();
            renderDeviceList(bridge);
            renderMidiInputList(bridge);
            renderMasterControls(bridge);
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
