# backends/fm_engine/CMakeLists.txt
#
# FmEngine バックエンド DLL のビルド定義。
#
# ─── 概要 ────────────────────────────────────────────────────────────────────
#   このディレクトリは FmEngineApi 互換の DLL (YMEngine.dll 等) のビルドを担う。
#   DLL 本体のソースは外部プロジェクト (FmEngineTest 等) に依存するため、
#   FMENGINE_SOURCE_DIR に外部ソースのパスを指定してビルドする。
#
#   外部ソースが未設定の場合はスタブのみをビルドし、configure は通過する。
#   実際の DLL は別途ビルドして engines/ ディレクトリに配置すること。
#
# ─── ビルド方法 ─────────────────────────────────────────────────────────────
#   1. 外部の FmEngine ソースを submodule 等で配置する
#      git submodule add <FmEngine-repo> backends/fm_engine/src
#
#   2. CMake 設定時にソースディレクトリを指定する (任意)
#      cmake --preset windows-vs2022-x64 -DFMENGINE_SOURCE_DIR=path/to/fmengine
#
#   外部ソースなしでも fitom_core 本体のビルドには影響しない。

set(FMENGINE_SOURCE_DIR "" CACHE PATH "FmEngine ソースディレクトリ (空=スキップ)")

# FmEngine ソースが存在する場合のみ DLL をビルドする
if(FMENGINE_SOURCE_DIR AND EXISTS "${FMENGINE_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "FmEngine バックエンド: ${FMENGINE_SOURCE_DIR}")
    add_subdirectory(${FMENGINE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/fmengine_build)
else()
    message(STATUS
        "FmEngine バックエンド: ソースなし (FMENGINE_SOURCE_DIR 未設定)"
        " — fitom_core のビルドには影響しません")
endif()

# fitom_core が参照する FmEngineApi.h のインクルードパスを公開
# ヘッダは plugin_sdk/include/fitom/IFmEnginePlugin.h を使用
# 外部の FmEngineApi.h が必要な場合は FMENGINE_INCLUDE_DIR を設定する
set(FMENGINE_INCLUDE_DIR "" CACHE PATH "FmEngineApi.h のディレクトリ (空=plugin_sdk のみ)")

if(FMENGINE_INCLUDE_DIR AND EXISTS "${FMENGINE_INCLUDE_DIR}")
    # fitom_core にインクルードパスを追加
    target_include_directories(fitom_core PUBLIC "${FMENGINE_INCLUDE_DIR}")
    message(STATUS "FmEngineApi.h: ${FMENGINE_INCLUDE_DIR}")
endif()
