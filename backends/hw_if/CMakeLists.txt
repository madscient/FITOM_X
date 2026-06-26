# backends/hw_if/CMakeLists.txt
#
# HW I/F バックエンド DLL のビルド定義。
#
# ─── 概要 ────────────────────────────────────────────────────────────────────
#   このディレクトリは IHWPlugin.h を実装した DLL (fitom_hw.dll 等) のビルドを担う。
#   DLL 本体は FitomIFTest (外部プロジェクト) に依存するため、
#   HWIF_SOURCE_DIR に外部ソースのパスを指定してビルドする。
#
#   外部ソースが未設定の場合はスタブのみをビルドし、configure は通過する。
#
# ─── ビルド方法 ─────────────────────────────────────────────────────────────
#   1. FitomIFTest を submodule として配置する
#      git submodule add <FitomIFTest-repo> backends/hw_if/FitomIFTest
#
#   2. CMake 設定時にソースディレクトリを指定する (任意)
#      cmake --preset windows-vs2022-x64 -DHWIF_SOURCE_DIR=path/to/FitomIFTest

set(HWIF_SOURCE_DIR "" CACHE PATH "FitomIFTest ソースディレクトリ (空=スキップ)")

if(HWIF_SOURCE_DIR AND EXISTS "${HWIF_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "HW I/F バックエンド: ${HWIF_SOURCE_DIR}")
    add_subdirectory(${HWIF_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/hwif_build)
else()
    message(STATUS
        "HW I/F バックエンド: ソースなし (HWIF_SOURCE_DIR 未設定)"
        " — fitom_core のビルドには影響しません")
endif()
