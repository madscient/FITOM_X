# apps/fitom_render/CMakeLists.txt
#
# 指定したプロファイルでSMFを演奏し、WAVファイルへ書き出すヘッドレスの変換器。
# SMFの読み込み・WAVの書き出し・オフラインの時間の進め方は、テストからも
# 使うので静的ライブラリに分けている(コアには依存しない)。

add_library(fitom_render_lib STATIC
    SmfReader.cpp
    WavWriter.cpp
    OfflineRenderer.cpp
)

target_include_directories(fitom_render_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(fitom_render
    main.cpp
)

target_link_libraries(fitom_render
    PRIVATE
        fitom_render_lib
        fitom_core
)

target_include_directories(fitom_render
    PRIVATE
        ${CMAKE_SOURCE_DIR}/plugin_sdk/include
)

# HWプラグインDLLと同じ ${CMAKE_BINARY_DIR}/bin に出力する(fitom_cli と同じ理由)。
set_target_properties(fitom_render PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
