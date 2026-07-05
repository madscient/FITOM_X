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

# MIDIバックエンドDLL(fitom_midi_winmm/fitom_midi_alsa)と同じ
# ${CMAKE_BINARY_DIR}/bin に出力する。fitom_cli の exeDir()(実行ファイル
# 自身のディレクトリ)基点でのMIDIバックエンド自動探索が機能するために、
# 実行ファイルとDLLが同じディレクトリに揃っている必要がある。
set_target_properties(fitom_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
