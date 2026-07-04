cmake_minimum_required(VERSION 3.20)
project(FITOM VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: 複数ソースファイルの並列コンパイル時、複数のcl.exeプロセスが
# 同一の.pdbファイルへ同時書き込みしようとして衝突する
# (C1041: プログラム データベースを開けません) ことがあるため、
# mspdbsrv.exe経由でPDB書き込みを直列化する /FS を全ターゲットに適用する。
# (FetchContentで取得するCatch2等、サブディレクトリ側のプロジェクトにも
#  確実に適用されるよう、トップレベルでグローバルに設定する)
if(MSVC)
    add_compile_options(/FS)
endif()

# -------------------------------------------------------
#  ビルドオプション
# -------------------------------------------------------
option(FITOM_BUILD_BACKEND_FMENGINE  "FmEngine バックエンド DLL をビルド"         ON)
option(FITOM_BUILD_BACKEND_HWIF      "HW I/F バックエンド DLL をビルド"           ON)
option(FITOM_BUILD_BACKEND_MIDI_WMS  "Windows MIDI Services バックエンド DLL"     OFF)  # Win のみ・WMS SDK 要
option(FITOM_BUILD_BACKEND_MIDI_ALSA "ALSA MIDI バックエンド DLL"                 OFF)  # Linux のみ
option(FITOM_GUI_MFC                 "MFC GUI shim をビルド (Windows のみ)"       OFF)
option(FITOM_GUI_QT                  "Qt6 GUI をビルド"                           OFF)
option(FITOM_BUILD_TESTS             "Catch2 テストをビルド"                       ON)
option(FITOM_USE_VCPKG_JSON          "nlohmann-json を submodule でなく vcpkg (find_package) から取得" OFF)

# -------------------------------------------------------
#  コンパイラ設定
# -------------------------------------------------------
if(MSVC)
    add_compile_options(/utf-8 /W4 /wd4100 /wd4505)
    add_compile_definitions(
        # 重要: vcpkgのboost-logバイナリはWindows 8以降を対象にビルドされて
        # おり、Boost.Logは対象Windowsバージョンに応じて内部名前空間を
        # 切り替える設計になっている (Windows7以下=v2_mt_nt6、
        # Windows8以降=v2_mt_nt62)。_WIN32_WINNT=0x0601(Windows7)のまま
        # だと、ヘッダー側がv2_mt_nt6名前空間でコンパイルされる一方、
        # リンクされるバイナリはv2_mt_nt62名前空間のシンボルしか持たず、
        # 全てのBoost.Logシンボルが未解決になる (LNK2001/LNK2019多発)。
        # vcpkgのビルド環境に合わせて0x0602(Windows8)以上を指定する。
        _WIN32_WINNT=0x0602
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# -------------------------------------------------------
#  依存ライブラリ
#  Boost: thread(named_mutex 等), format, log, interprocess(named_mutex)
#         asio / system は使用箇所なし (旧 RtAudio 経由コードの残骸) のため削除。
#         filesystem / algorithm / foreach は std で代替済みのため不要
#
#  nlohmann-json は git submodule (third_party/nlohmann-json) で取得する。
#  vcpkg は必須ではなくなった。Windows で boost を手元に用意しづらい場合の
#  選択肢としてのみ vcpkg.json を残している (任意)。
# -------------------------------------------------------

# nlohmann-json: 既定は git submodule (ヘッダオンリー)。
#   git submodule add https://github.com/nlohmann/json.git third_party/nlohmann-json
# vcpkg 経由で取得したい場合は -DFITOM_USE_VCPKG_JSON=ON を指定する。
if(FITOM_USE_VCPKG_JSON)
    find_package(nlohmann_json CONFIG REQUIRED)
else()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/nlohmann-json/CMakeLists.txt")
        message(FATAL_ERROR
            "third_party/nlohmann-json が見つかりません。\n"
            "  git submodule update --init --recursive\n"
            "を実行してください。(vcpkg を使う場合は代わりに\n"
            "-DFITOM_USE_VCPKG_JSON=ON を指定してください)")
    endif()
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    add_subdirectory(third_party/nlohmann-json)
endif()
# nlohmann/json 公式 CMakeLists.txt が nlohmann_json::nlohmann_json という
# INTERFACE ターゲットを定義するため、vcpkg 版と同じ名前でリンクできる。

find_package(Boost 1.71 REQUIRED COMPONENTS
    thread          # boost::thread
    log             # Boost.Log (ロギング)
    log_setup
)
# interprocess::named_mutex はヘッダオンリーのため COMPONENTS 不要 (Boost::headers に含まれる)
# Boost.Log は ヘッダオンリーでないため DLL/静的リンクを明示
add_compile_definitions(BOOST_LOG_DYN_LINK)

# -------------------------------------------------------
#  サブプロジェクト
# -------------------------------------------------------

# プラグイン SDK (ヘッダのみ: IMidiPlugin.h / IHWPlugin.h / IFmEnginePlugin.h)
add_subdirectory(plugin_sdk)

# コアライブラリ (fitom_core static lib)
add_subdirectory(core)

# バックエンド DLL (各自が fitom_core に非依存の独立 DLL)
if(FITOM_BUILD_BACKEND_FMENGINE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/fm_engine/CMakeLists.txt")
        add_subdirectory(backends/fm_engine)
    else()
        message(WARNING
            "FITOM_BUILD_BACKEND_FMENGINE=ON ですが backends/fm_engine/ が存在しません。"
            " FmEngine バックエンド DLL のビルドをスキップします。"
            " FmEngineApi 互換の DLL を別途ビルドして engines/ ディレクトリに配置してください。")
    endif()
endif()
if(FITOM_BUILD_BACKEND_HWIF)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/hw_if/CMakeLists.txt")
        add_subdirectory(backends/hw_if)
    else()
        message(WARNING
            "FITOM_BUILD_BACKEND_HWIF=ON ですが backends/hw_if/ が存在しません。"
            " HW I/F バックエンド DLL のビルドをスキップします。"
            " FitomIFTest の共有ライブラリを別途ビルドして配置してください。")
    endif()
endif()
if(FITOM_BUILD_BACKEND_MIDI_WMS)
    if(NOT WIN32)
        message(FATAL_ERROR "FITOM_BUILD_BACKEND_MIDI_WMS は Windows のみ")
    endif()
    add_subdirectory(backends/midi_wms)
endif()
if(FITOM_BUILD_BACKEND_MIDI_ALSA)
    if(NOT UNIX OR APPLE)
        message(FATAL_ERROR "FITOM_BUILD_BACKEND_MIDI_ALSA は Linux のみ")
    endif()
    add_subdirectory(backends/midi_alsa)
endif()

# GUI
if(FITOM_GUI_MFC)
    if(NOT WIN32)
        message(FATAL_ERROR "FITOM_GUI_MFC は Windows のみ")
    endif()
    add_subdirectory(gui/mfc_shim)
endif()
if(FITOM_GUI_QT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gui/qt/CMakeLists.txt")
        add_subdirectory(gui/qt)
    else()
        message(WARNING "FITOM_GUI_QT=ON ですが gui/qt/ が存在しません。スキップします。")
    endif()
endif()

# テスト
if(FITOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
