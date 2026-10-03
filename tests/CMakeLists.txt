# tests/CMakeLists.txt

find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
    # Catch2 が vcpkg にない場合は FetchContent でも取得可能
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.4.0
    )
    FetchContent_MakeAvailable(Catch2)
endif()

include(CTest)
include(Catch)

# -------------------------------------------------------
#  テストターゲット
# -------------------------------------------------------
add_executable(fitom_tests
    test_plugin_loader.cpp
    test_config.cpp
    test_soundevice_freq.cpp
    test_midi_tuning.cpp
    test_opl3_carrier.cpp
    test_opz_keyon.cpp
    test_voiceprocessor_lfo.cpp
    test_voiceprocessor_tl.cpp
    test_psg_volume.cpp
    test_dsg.cpp
    test_ssgs.cpp
    test_silence_bank.cpp
    test_y8960.cpp
    test_hwport.cpp
    test_part_gain.cpp
)

# -------------------------------------------------------
#  HWPort テスト用の検証用 HW プラグイン
# -------------------------------------------------------
# 同じソースを、部位ゲインの組のエクスポート状態だけ変えて3通りにビルドする
# (stub_hwplugin/StubHWPlugin.cpp 冒頭参照)。DLL のパスはコンパイル定義で
# テストへ渡す(テストのソースにビルドディレクトリの構成を持ち込まない)。
function(fitom_add_stub_hwplugin target macro)
    add_library(${target} SHARED stub_hwplugin/StubHWPlugin.cpp)
    target_compile_definitions(${target} PRIVATE FITOM_HW_PLUGIN_EXPORTS ${ARGN})
    target_link_libraries(${target} PRIVATE
        fitom_plugin_sdk
        nlohmann_json::nlohmann_json)
    add_dependencies(fitom_tests ${target})
    target_compile_definitions(fitom_tests PRIVATE
        "${macro}=\"$<TARGET_FILE:${target}>\"")
endfunction()

fitom_add_stub_hwplugin(fitom_test_hwplugin_parts     FITOM_TEST_HWPLUGIN_PARTS)
fitom_add_stub_hwplugin(fitom_test_hwplugin_no_parts  FITOM_TEST_HWPLUGIN_NO_PARTS
    STUB_HWPLUGIN_NO_PARTS)
fitom_add_stub_hwplugin(fitom_test_hwplugin_head_only FITOM_TEST_HWPLUGIN_HEAD_ONLY
    STUB_HWPLUGIN_HEAD_ONLY)

# 内部用MIDIパイプ(fitom_midi_pipe)の回帰テスト。DLL自体が
# FITOM_BUILD_BACKEND_MIDI_PIPE=OFF(既定)ではビルドされないため、
# このソース・依存関係もそのときは追加しない(既存テストスイートへの
# 影響はゼロ)。
if(FITOM_BUILD_BACKEND_MIDI_PIPE)
    target_sources(fitom_tests PRIVATE test_midi_pipe.cpp)
    add_dependencies(fitom_tests fitom_midi_pipe)
endif()

target_link_libraries(fitom_tests
    PRIVATE
        fitom_core
        Catch2::Catch2WithMain
        # fitom_core は Boost::log を PUBLIC でリンクしているため理論上は
        # 推移的に解決されるはずだが、Visual Studioジェネレーター使用時、
        # 静的ライブラリ経由の推移的リンク解決が正しく行われず
        # (boost::log::v2_mt_nt6::* の LNK2001/LNK2019 多発)、実行ファイル
        # リンク時に boost_log の import libraryが見つからない不整合が
        #発生することがあるため、ここでも明示的にリンクする。
        Boost::log
        Boost::log_setup
)

catch_discover_tests(fitom_tests)
