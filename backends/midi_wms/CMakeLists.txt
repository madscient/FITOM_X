# backends/midi_wms/CMakeLists.txt
# Windows MIDI Services バックエンド DLL
#
# ─── ビルド前提 ──────────────────────────────────────────────────────────────
#   1. Visual Studio 2022 + Windows SDK 10.0.20348+
#   2. NuGet パッケージを local feed に追加済み:
#        Microsoft.Windows.Devices.Midi2
#        Microsoft.Windows.CppWinRT
#   3. NuGet restore の実行 (VS が自動実行、または手動)
#   4. 64bit のみ
#
# ─── WMS SDK の組み込み方法 ──────────────────────────────────────────────────
#   CMake だけでは C++/WinRT プロジェクション生成が困難なため、
#   このサブプロジェクトは Visual Studio でビルドすることを推奨する。
#   CMake から呼ぶ場合は cppwinrt.exe のカスタムコマンドが必要 (下記参照)。

cmake_minimum_required(VERSION 3.20)

if(NOT WIN32)
    message(FATAL_ERROR "midi_wms は Windows のみ")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Windows MIDI Services は 64bit のみサポートします")
endif()

# ─── C++/WinRT プロジェクションヘッダの生成 ──────────────────────────────────
# cppwinrt.exe は NuGet: Microsoft.Windows.CppWinRT に含まれる
# MIDI2_WINMD: NuGet パッケージから取り出した .winmd のパス
set(MIDI2_WINMD "" CACHE FILEPATH
    "Microsoft.Windows.Devices.Midi2.winmd のパス (NuGet パッケージから取得)")

set(MIDI2_SDK_INCLUDE "" CACHE PATH
    "WMS SDK インクルードディレクトリ (NuGet パッケージ展開先)")

if(NOT EXISTS "${MIDI2_SDK_INCLUDE}/winrt/Microsoft.Windows.Devices.Midi2.h")
    if(MIDI2_WINMD AND EXISTS "${MIDI2_WINMD}")
        # cppwinrt.exe でプロジェクションヘッダを生成
        find_program(CPPWINRT_EXE cppwinrt HINTS "$ENV{NUGET_PACKAGES}/microsoft.windows.cppwinrt")
        if(CPPWINRT_EXE)
            set(_PROJ_OUT "${CMAKE_CURRENT_BINARY_DIR}/winrt_projection")
            file(MAKE_DIRECTORY "${_PROJ_OUT}")
            execute_process(
                COMMAND "${CPPWINRT_EXE}" -input "${MIDI2_WINMD}" -output "${_PROJ_OUT}"
                RESULT_VARIABLE _CPPWINRT_RESULT
            )
            if(_CPPWINRT_RESULT EQUAL 0)
                set(MIDI2_SDK_INCLUDE "${_PROJ_OUT}" CACHE PATH "" FORCE)
            else()
                message(FATAL_ERROR "cppwinrt.exe の実行に失敗しました")
            endif()
        endif()
    endif()
    if(NOT EXISTS "${MIDI2_SDK_INCLUDE}/winrt/Microsoft.Windows.Devices.Midi2.h")
        message(FATAL_ERROR
            "WMS SDK ヘッダが見つかりません。\n"
            "  MIDI2_SDK_INCLUDE または MIDI2_WINMD を指定してください。\n"
            "  NuGet: Microsoft.Windows.Devices.Midi2 を local feed からインストールし、\n"
            "  展開されたパッケージの include/ ディレクトリを MIDI2_SDK_INCLUDE に設定。")
    endif()
endif()

# ─── DLL ターゲット ──────────────────────────────────────────────────────────
add_library(fitom_midi_wms SHARED src/MidiWMS.cpp)

target_compile_definitions(fitom_midi_wms PRIVATE
    FITOM_MIDI_PLUGIN_EXPORTS
    WINRT_LEAN_AND_MEAN
)

target_include_directories(fitom_midi_wms PRIVATE
    "${MIDI2_SDK_INCLUDE}"
    ${CMAKE_SOURCE_DIR}/plugin_sdk/include
)

# C++/WinRT は C++20 を要求
set_property(TARGET fitom_midi_wms PROPERTY CXX_STANDARD 20)

target_link_libraries(fitom_midi_wms PRIVATE
    nlohmann_json::nlohmann_json
    windowsapp          # WinRT サポートライブラリ
    runtimeobject       # RoInitialize 等
)

# NuGet の WMS Runtime DLL をコピー
set(MIDI2_RUNTIME_DLL "" CACHE FILEPATH
    "Microsoft.Windows.Devices.Midi2.dll のパス (NuGet パッケージから取得)")
if(MIDI2_RUNTIME_DLL AND EXISTS "${MIDI2_RUNTIME_DLL}")
    add_custom_command(TARGET fitom_midi_wms POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MIDI2_RUNTIME_DLL}"
            "$<TARGET_FILE_DIR:fitom_midi_wms>")
endif()

set_target_properties(fitom_midi_wms PROPERTIES
    OUTPUT_NAME "fitom_midi_wms"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
