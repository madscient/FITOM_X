# apps/fitom_cli/CMakeLists.txt
#
# コア機能をGUI実装前に評価するための、テキストコンソール版CLIツール。
# 実MIDIデバイスからの入力を受け取り、16 MIDIチャンネルの発音状況を
# top コマンド風に定期再描画するモニター画面を表示する。

add_executable(fitom_cli
    main.cpp
)

target_link_libraries(fitom_cli
    PRIVATE
        fitom_core
)

target_include_directories(fitom_cli
    PRIVATE
        ${CMAKE_SOURCE_DIR}/plugin_sdk/include
)
