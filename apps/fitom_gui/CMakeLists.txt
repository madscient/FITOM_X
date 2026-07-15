# apps/fitom_gui/CMakeLists.txt
#
# Dear ImGuiベースのGUIアプリケーション。fitom_core には直接触れず、
# gui/bridge (FITOMBridge) 経由でのみコアにアクセスする。
#
# 現時点ではDear ImGui本体・レンダラー/ウィンドウバックエンド(GLFW+OpenGL3
# 等)はまだ導入していないプレースホルダ状態。main.cpp は
# fitom_gui_bridge とのリンクが正しく通ることの確認用。

add_executable(fitom_gui
    main.cpp
)

target_link_libraries(fitom_gui
    PRIVATE
        fitom_gui_bridge
)

# fitom_cli と同じ場所に出力する(MIDIバックエンドDLLの自動探索のため)。
set_target_properties(fitom_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
