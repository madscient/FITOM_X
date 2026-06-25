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
)

catch_discover_tests(fitom_tests)
