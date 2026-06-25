# core/CMakeLists.txt

# ─── RtAudio ─────────────────────────────────────────────────────────────────
set(RTAUDIO_DIR "${CMAKE_SOURCE_DIR}/extern/rtaudio" CACHE PATH "RtAudio ルートディレクトリ")
if(EXISTS "${RTAUDIO_DIR}/CMakeLists.txt")
    set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RTAUDIO_DIR} ${CMAKE_BINARY_DIR}/rtaudio EXCLUDE_FROM_ALL)
    set(FITOM_HAS_RTAUDIO TRUE)
else()
    find_package(RtAudio CONFIG QUIET)
    set(FITOM_HAS_RTAUDIO ${RtAudio_FOUND})
endif()

# ─── FmEngineApi ヘッダ ────────────────────────────────────────────────────────
set(FMENGINE_HEADER_DIR "${CMAKE_SOURCE_DIR}/backends/fm_engine/include"
    CACHE PATH "FmEngineApi.h のディレクトリ")

# ─── ソースファイル ────────────────────────────────────────────────────────────
set(FITOM_CORE_SOURCES
    # インフラ層
    src/IPort.cpp
    src/Log.cpp
    src/VolumeUtils.cpp
    src/FmEnginePort.cpp
    src/HWPort.cpp
    src/MidiManager.cpp
    src/Config.cpp
    src/PatchManager.cpp
    src/VoiceProcessor.cpp
    src/DeviceFactory.cpp

    # 音源デバイス共通
    src/SoundDevImpl.cpp

    # チップドライバ
    src/OPN_new.cpp
    src/OPN2_new.cpp
    src/OPM_new.cpp
    src/OPL_new.cpp
    src/OPLL_new.cpp
    src/PSG_new.cpp
    src/MultiDev_new.cpp
    src/ADPCM_new.cpp

    # MIDI 処理
    src/MidiCh.cpp

    # コアシングルトン
    src/CFITOM.cpp

    # F-number テーブル (変更不要)
    src/Fnum.cpp
)

if(FITOM_HAS_RTAUDIO)
    list(APPEND FITOM_CORE_SOURCES src/AudioEngine.cpp)
endif()

# ─── ターゲット ────────────────────────────────────────────────────────────────
add_library(fitom_core STATIC ${FITOM_CORE_SOURCES})

target_include_directories(fitom_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/plugin_sdk/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(EXISTS "${FMENGINE_HEADER_DIR}/FmEngineApi.h")
    target_include_directories(fitom_core PUBLIC ${FMENGINE_HEADER_DIR})
endif()

target_compile_definitions(fitom_core PUBLIC BOOST_LOG_DYN_LINK)

target_link_libraries(fitom_core
    PUBLIC
        nlohmann_json::nlohmann_json
        Boost::log
        Boost::log_setup
        Boost::thread
        Boost::system
        Boost::headers      # boost-format はヘッダオンリー (Boost::headers に含まれる)
)

if(FITOM_HAS_RTAUDIO)
    target_link_libraries(fitom_core PUBLIC rtaudio)
    target_compile_definitions(fitom_core PUBLIC FITOM_HAS_RTAUDIO)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(fitom_core PRIVATE dl)
endif()
