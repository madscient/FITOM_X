# core/CMakeLists.txt

# RtAudio は fitom_core から削除済み。
# オーディオ出力は hwif 互換 DLL (fitom_fmhwif) が内部で管理する。
# → AudioEngine.h / AudioEngine.cpp は廃止（削除対象）。

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
target_link_libraries(fitom_core
    PUBLIC
        nlohmann_json::nlohmann_json
        Boost::headers      # boost-format 等のヘッダオンリーコンポーネントを含む
        Boost::log
        Boost::log_setup
        Boost::thread
        Boost::system
)



if(UNIX AND NOT APPLE)
    target_link_libraries(fitom_core PRIVATE dl)
endif()

# /FS: 並列コンパイル時の .pdb 書き込み競合を防ぐ (MSVC のみ)
if(MSVC)
    target_compile_options(fitom_core PRIVATE /FS)
endif()
