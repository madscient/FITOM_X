# backends/midi_rtmidi/CMakeLists.txt
#
# RtMidi (https://github.com/thestk/rtmidi) を使った単一クロスプラットフォーム
# MIDIバックエンドDLL。旧 backends/midi_wms (Windows MIDI Services) /
# backends/midi_winmm (WinMM) / backends/midi_alsa (ALSA) の3実装を統合し、
# Windows(WinMM API)/Linux(ALSA)/macOS(CoreMIDI) を1ソースでカバーする。
#
# RtMidi 本体は third_party/rtmidi に git submodule として配置済み
# (git submodule update --init --recursive で取得)。

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/rtmidi/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/rtmidi が見つかりません。\n"
        "  git submodule update --init --recursive\n"
        "を実行してください。")
endif()

# RtMidi 自身が cmake_minimum_required(VERSION 3.24) を要求するため、
# このターゲットを有効化する場合はビルド環境側もCMake 3.24以上が必要になる。

# ─── RtMidi 本体を静的リンクでビルドする ─────────────────────────────────────
# RTMIDI_BUILD_SHARED_LIBS/RTMIDI_BUILD_STATIC_LIBS はRtMidi自身が
# サブプロジェクトとして組み込まれる際の上書き用に用意している専用変数
# (汎用の BUILD_SHARED_LIBS を弄ると他のadd_subdirectory<glfw/imgui等>に
#  影響しかねないため、専用変数の方を使う)。
# add_subdirectory() は新しいディレクトリ変数スコープを作るため、ここで
# 通常変数として設定しても呼び出し元や他のadd_subdirectory呼び出しには
# 影響が漏れない。
set(RTMIDI_BUILD_SHARED_LIBS OFF)

# 静的ライブラリを共有ライブラリ(fitom_midi_rtmidi.dll/.so)に組み込むため、
# Linux/macOSではPIC必須(Windowsでは無関係・無害)。
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# RtMidi自身のサンプル実行ファイル(cmidiin/midiprobe等)・ctestターゲットは
# 不要なため無効化する。
set(RTMIDI_BUILD_TESTING OFF)

# Linux開発機に偶然JACKが入っていても、意図せずJACK依存を持ち込まないよう
# 明示的に無効化する(既存backends/midi_alsaと同じくALSAのみを土台にする)。
# なお RtMidi 本体の CMakeLists.txt は ALSA サポート自体を
# find_package(ALSA) の可否だけで自動的に有効化する(RTMIDI_API_ALSA
# オプションの値は実際には参照されない)ため、ALSA側は明示設定不要。
set(RTMIDI_API_JACK OFF)

# CoreMIDI(macOS)・WinMM(Windows)はRtMidi側の既定(APPLE/WIN32から自動判定)
# のままで問題ないため明示指定不要。

add_subdirectory(
    "${CMAKE_SOURCE_DIR}/third_party/rtmidi"
    "${CMAKE_CURRENT_BINARY_DIR}/rtmidi_build")

if(MSVC)
    # RTMIDI_DLL_PUBLIC(__declspec(dllexport/import))は静的リンクでは
    # 無意味だが、RtMidi側のCMakeLists.txtが静的/共有を問わず同じマクロを
    # 使うため、STLメンバのDLL境界越え警告(C4251/C4275)が出る。静的
    # ライブラリのため実害はない。C4245はrtmidi_c.cpp(C API、本バックエンド
    # では未使用)内のsigned/unsigned変換警告。いずれもvendored codeのため
    # 抑制する。
    target_compile_options(rtmidi PRIVATE /wd4251 /wd4275 /wd4245)
endif()

# ─── DLL ターゲット ──────────────────────────────────────────────────────────
add_library(fitom_midi_rtmidi SHARED src/MidiRtMidi.cpp)

target_compile_definitions(fitom_midi_rtmidi PRIVATE FITOM_MIDI_PLUGIN_EXPORTS)

target_include_directories(fitom_midi_rtmidi PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin_sdk/include)

target_link_libraries(fitom_midi_rtmidi PRIVATE
    rtmidi
    nlohmann_json::nlohmann_json)

set_target_properties(fitom_midi_rtmidi PROPERTIES
    OUTPUT_NAME "fitom_midi_rtmidi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
