// apps/fitom_gui/main.cpp
//
// Dear ImGuiベースのGUIアプリケーション エントリポイント。
// 現時点ではプレースホルダ(ビルド配線の確認用)。
//
// TODO: Dear ImGui + GLFW/OpenGL3 (または選定したバックエンド) を導入し、
// FITOMBridge を介してウィンドウにデバイス一覧・パッチ一覧等を描画する。

#include "FITOMBridge.h"
#include <cstdio>

int main()
{
    std::printf("fitom_gui: placeholder (Dear ImGui integration pending)\n");

    FITOMBridge& bridge = FITOMBridge::instance();
    (void)bridge; // ビルド・リンク確認用。実際の初期化はImGui導入後に行う。

    return 0;
}
