# core/CMakeLists.txt

# ─── RtAudio ─────────────────────────────────────────────────────────────────
# extern/rtaudio/ を git submodule として配置した場合に自動で有効になる。
# 追加コマンド:
#   git submodule add https://github.com/thestk/rtaudio extern/rtaudio
#   git submodule update --init --recursive
# extern/rtaudio/ を git submodule として配置した場合に自動で有効になる。
# 追加コマンド:
#   git submodule add https://github.com/thestk/rtaudio extern/rtaudio
#   git submodule update --init --recursive
set(RTAUDIO_DIR "${CMAKE_SOURCE_DIR}/extern/rtaudio" CACHE PATH "RtAudio ルートディレクトリ")
if(EXISTS "${RTAUDIO_DIR}/CMakeLists.txt")
    set(RTAUDIO_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
    set(RTAUDIO_BUILD_STATIC_LIBS ON  CACHE BOOL "" FORCE)
    add_subdirectory(${RTAUDIO_DIR} ${CMAKE_BINARY_DIR}/rtaudio EXCLUDE_FROM_ALL)
    set(FITOM_HAS_RTAUDIO TRUE)
    message(STATUS "RtAudio: found at ${RTAUDIO_DIR}")
    message(STATUS "RtAudio: found at ${RTAUDIO_DIR}")
else()
    set(FITOM_HAS_RTAUDIO FALSE)
    message(STATUS "RtAudio: not found (extern/rtaudio/ 未配置) — AudioEngine 無効")
    set(FITOM_HAS_RTAUDIO FALSE)
    message(STATUS "RtAudio: not found (extern/rtaudio/ 未配置) — AudioEngine 無効")
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

# AudioEngine.cpp は FITOM_HAS_RTAUDIO が有効な場合のみビルドする
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

# Boost::format はヘッダオンリーのため Boost::headers を使う
# (vcpkg では独立した Boost::format ターゲットは存在しない)
# Boost::format はヘッダオンリーのため Boost::headers を使う
# (vcpkg では独立した Boost::format ターゲットは存在しない)
target_link_libraries(fitom_core
    PUBLIC
        nlohmann_json::nlohmann_json
        Boost::headers      # boost-format 等のヘッダオンリーコンポーネントを含む
        Boost::headers      # boost-format 等のヘッダオンリーコンポーネントを含む
        Boost::log
        Boost::log_setup
        Boost::thread
        Boost::system
)

if(FITOM_HAS_RTAUDIO)
    # AudioEngine を別 object library にして PDB 競合を回避
    add_library(fitom_audio_engine OBJECT src/AudioEngine.cpp)
    target_include_directories(fitom_audio_engine
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/plugin_sdk/include
    )
    if(EXISTS "${FMENGINE_HEADER_DIR}/FmEngineApi.h")
        target_include_directories(fitom_audio_engine
            PRIVATE ${FMENGINE_HEADER_DIR})
    endif()
    target_compile_definitions(fitom_audio_engine
        PRIVATE BOOST_LOG_DYN_LINK FITOM_HAS_RTAUDIO)
    if(MSVC)
        target_compile_options(fitom_audio_engine PRIVATE /FS)
    endif()
    target_link_libraries(fitom_core PUBLIC
        $<TARGET_OBJECTS:fitom_audio_engine>
        rtaudio)
    target_compile_definitions(fitom_core PUBLIC FITOM_HAS_RTAUDIO)
    # RtAudio のインクルードパスを fitom_core に伝える
    target_include_directories(fitom_core PRIVATE
        ${RTAUDIO_DIR})
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(fitom_core PRIVATE dl)
endif()

# /FS: 並列コンパイル時の .pdb 書き込み競合を防ぐ (MSVC のみ)
if(MSVC)
    target_compile_options(fitom_core PRIVATE /FS)
endif()

# /FS: 並列コンパイル時の .pdb 書き込み競合を防ぐ (MSVC のみ)
if(MSVC)
    target_compile_options(fitom_core PRIVATE /FS)
endif()
