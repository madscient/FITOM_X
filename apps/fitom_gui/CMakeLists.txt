# apps/fitom_gui/CMakeLists.txt
#
# Dear ImGuiベースのGUIアプリケーション。fitom_core には直接触れず、
# gui/bridge (FITOMBridge) 経由でのみコアにアクセスする。
# バックエンドは GLFW + OpenGL3 (third_party/imgui, third_party/glfw)。

add_executable(fitom_gui
    main.cpp
    ChSettingsDialog.cpp
    PatchPickerDialog.cpp
    MidiPortSettingsDialog.cpp
    SystemSettingsDialog.cpp
    RegisterDumpWindow.cpp
    LevelMeterPanel.cpp
)

target_link_libraries(fitom_gui
    PRIVATE
        fitom_gui_bridge
        imgui
)

# fitom_cli と同じ場所に出力する(MIDIバックエンドDLLの自動探索のため)。
set_target_properties(fitom_gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# assets/ (日本語フォント等) を実行ファイルと同じディレクトリへコピーする。
# main.cpp の loadJapaneseFont() は exeDir()/assets/fonts/... を第一候補
# として探索する。
add_custom_command(TARGET fitom_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:fitom_gui>/assets"
    COMMENT "Copying apps/fitom_gui/assets to output directory"
)
