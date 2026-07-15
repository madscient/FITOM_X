# gui/bridge/CMakeLists.txt
# GUI ブリッジ (UIフレームワーク非依存、Windows/Linux/macOS共通)
#
# FITOMBridge は fitom_core への唯一のアクセス経路であり、GUI側の
# コード(apps/fitom_gui 等)はこのライブラリ経由でのみコアに触れる。
# 特定のUIツールキット(MFC/Qt/Dear ImGui等)への依存は一切持たない。

add_library(fitom_gui_bridge STATIC
    FITOMBridge.cpp
)

target_include_directories(fitom_gui_bridge
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(fitom_gui_bridge
    PUBLIC  fitom_core
)
