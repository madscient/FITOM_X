# plugin_sdk/CMakeLists.txt
# ヘッダオンリー INTERFACE ターゲット
# fitom_core と各バックエンド DLL の両方がこれを使う
add_library(fitom_plugin_sdk INTERFACE)
target_include_directories(fitom_plugin_sdk INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
