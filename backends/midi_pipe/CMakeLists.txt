# backends/midi_pipe/CMakeLists.txt
# 内部用MIDIパイプ バックエンド DLL (クロスプラットフォーム)

add_library(fitom_midi_pipe SHARED src/MidiPipe.cpp)

target_compile_definitions(fitom_midi_pipe PRIVATE FITOM_MIDI_PLUGIN_EXPORTS)

target_include_directories(fitom_midi_pipe PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk/include)

target_link_libraries(fitom_midi_pipe PRIVATE
    nlohmann_json::nlohmann_json)

set_target_properties(fitom_midi_pipe PROPERTIES
    OUTPUT_NAME "fitom_midi_pipe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
