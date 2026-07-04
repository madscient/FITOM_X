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
    test_audio_engine.cpp
)

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
