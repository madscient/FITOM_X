# 上流のvcpkgのrtmidiポートは6.0.0までしか無く、FITOM_Xが要る6.1.0の修正
# (cancelCallback()と入力スレッドの競合)を含まないため、上流のポート
# (microsoft/vcpkg 0df93a05b9 の ports/rtmidi)を写して版とSHA512を変え、
# RTMIDI_API_WMSのOFFを足したものをoverlayとして持つ。上流のポートが6.1.0以上に
# なったら、このディレクトリとvcpkg.jsonのoverlay-portsの指定を消す。

# Upstream uses CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS, which causes issues
# https://github.com/thestk/rtmidi/blob/4.0.0/CMakeLists.txt#L20
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO thestk/rtmidi
    REF "${VERSION}"
    SHA512 4f557581a61765ee4114e8f33c6e0c8849fa575ab7a5507ff711f799dbb284743e309823763d2b5509c14cf86278c3c03a375574a8491a45f0e6d51a144888b8
    HEAD_REF master
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        alsa RTMIDI_API_ALSA
)

# 6.1.0で加わったWindows MIDI Servicesのバックエンドは、アプリ側にDLLの同梱と
# C++20が要る。FITOMはWindowsではWinMMだけを土台にするため、RtMidi側の既定
# (OFF)が将来変わっても持ち込まないよう明示的に無効化する。
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DRTMIDI_API_JACK=OFF
        -DRTMIDI_API_WMS=OFF
        -DRTMIDI_BUILD_TESTING=OFF
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_fixup_pkgconfig()

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
