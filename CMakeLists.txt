cmake_minimum_required(VERSION 3.20)
project(FITOM VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------------------------------------------
#  ビルドオプション
# -------------------------------------------------------
option(FITOM_BUILD_BACKEND_FMENGINE  "FmEngine バックエンド DLL をビルド"         ON)
option(FITOM_BUILD_BACKEND_HWIF      "HW I/F バックエンド DLL をビルド"           ON)
option(FITOM_BUILD_BACKEND_MIDI_WMS  "Windows MIDI Services バックエンド DLL"     OFF)  # Win のみ・WMS SDK 要
option(FITOM_BUILD_BACKEND_MIDI_ALSA "ALSA MIDI バックエンド DLL"                 OFF)  # Linux のみ
option(FITOM_GUI_MFC                 "MFC GUI shim をビルド (Windows のみ)"       OFF)
option(FITOM_GUI_QT                  "Qt6 GUI をビルド"                           OFF)
option(FITOM_BUILD_TESTS             "Catch2 テストをビルド"                       ON)

# -------------------------------------------------------
#  コンパイラ設定
# -------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8 /W4 /wd4100 /wd4505)
    add_compile_definitions(
        _WIN32_WINNT=0x0601
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# -------------------------------------------------------
#  依存ライブラリ
#  Boost: asio(SPFMController), thread(named_mutex), format, log
#         filesystem / algorithm / foreach は std で代替済みのため不要
# -------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost 1.71 REQUIRED COMPONENTS
    system          # Boost.Asio (SPFMController が要求)
    thread          # named_mutex (プロセス間排他)
    log             # Boost.Log (ロギング)
    log_setup
)
# Boost.Log は ヘッダオンリーでないため DLL/静的リンクを明示
add_compile_definitions(BOOST_LOG_DYN_LINK)

# -------------------------------------------------------
#  サブプロジェクト
# -------------------------------------------------------

# プラグイン SDK (ヘッダのみ: IMidiPlugin.h / IHWPlugin.h / IFmEnginePlugin.h)
add_subdirectory(plugin_sdk)

# コアライブラリ (fitom_core static lib)
add_subdirectory(core)

# バックエンド DLL (各自が fitom_core に非依存の独立 DLL)
if(FITOM_BUILD_BACKEND_FMENGINE)
    add_subdirectory(backends/fm_engine)
endif()
if(FITOM_BUILD_BACKEND_HWIF)
    add_subdirectory(backends/hw_if)
endif()
if(FITOM_BUILD_BACKEND_MIDI_WMS)
    if(NOT WIN32)
        message(FATAL_ERROR "FITOM_BUILD_BACKEND_MIDI_WMS は Windows のみ")
    endif()
    add_subdirectory(backends/midi_wms)
endif()
if(FITOM_BUILD_BACKEND_MIDI_ALSA)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "FITOM_BUILD_BACKEND_MIDI_ALSA は Linux のみ")
    endif()
    add_subdirectory(backends/midi_alsa)
endif()

# GUI
if(FITOM_GUI_MFC)
    if(NOT WIN32)
        message(FATAL_ERROR "FITOM_GUI_MFC は Windows のみ")
    endif()
    add_subdirectory(gui/mfc_shim)
endif()
if(FITOM_GUI_QT)
    add_subdirectory(gui/qt)
endif()

# テスト
if(FITOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
