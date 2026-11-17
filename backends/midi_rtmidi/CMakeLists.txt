# backends/midi_rtmidi/CMakeLists.txt
#
# RtMidi (https://github.com/thestk/rtmidi) を使った単一クロスプラットフォーム
# MIDIバックエンドDLL。Windows(WinMM API)/Linux(ALSA)/macOS(CoreMIDI) を
# 1ソースでカバーする。
#
# RtMidi 本体は vcpkg から静的ライブラリとして取得する(vcpkg-ports/rtmidi の
# overlay port。JACKとWindows MIDI ServicesのバックエンドはOFF、ALSAはLinux
# だけで有効)。

find_package(rtmidi CONFIG REQUIRED)

# ─── DLL ターゲット ──────────────────────────────────────────────────────────
add_library(fitom_midi_rtmidi SHARED src/MidiRtMidi.cpp)

target_compile_definitions(fitom_midi_rtmidi PRIVATE FITOM_MIDI_PLUGIN_EXPORTS)

target_include_directories(fitom_midi_rtmidi PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk/include)

target_link_libraries(fitom_midi_rtmidi PRIVATE
    RtMidi::rtmidi
    nlohmann_json::nlohmann_json)

set_target_properties(fitom_midi_rtmidi PROPERTIES
    OUTPUT_NAME "fitom_midi_rtmidi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
