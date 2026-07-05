# backends/midi_alsa/CMakeLists.txt

find_package(ALSA REQUIRED)

add_library(fitom_midi_alsa SHARED src/MidiALSA.cpp)

target_compile_definitions(fitom_midi_alsa PRIVATE FITOM_MIDI_PLUGIN_EXPORTS)

target_include_directories(fitom_midi_alsa PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk/include)

target_link_libraries(fitom_midi_alsa PRIVATE
    ALSA::ALSA
    nlohmann_json::nlohmann_json)

set_target_properties(fitom_midi_alsa PROPERTIES
    OUTPUT_NAME "fitom_midi_alsa"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
