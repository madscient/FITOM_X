# backends/midi_winmm/CMakeLists.txt
#
# クラシック WinMM API (winmm.lib) による薄いMIDIバックエンド。
# C++/WinRTを必要とする backends/midi_wms/ (Windows MIDI Services) とは
# 別物。ランタイムの追加インストール不要で、Windows全バージョンで
# そのまま動作する。

add_library(fitom_midi_winmm SHARED src/MidiWinMM.cpp)

target_compile_definitions(fitom_midi_winmm PRIVATE FITOM_MIDI_PLUGIN_EXPORTS)

target_include_directories(fitom_midi_winmm PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk/include)

target_link_libraries(fitom_midi_winmm PRIVATE
    winmm
    nlohmann_json::nlohmann_json)

set_target_properties(fitom_midi_winmm PROPERTIES
    OUTPUT_NAME "fitom_midi_winmm"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
