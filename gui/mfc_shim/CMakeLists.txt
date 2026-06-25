# gui/mfc_shim/CMakeLists.txt
# MFC GUI ブリッジ (Windows のみ)

if(NOT WIN32)
    message(FATAL_ERROR "mfc_shim は Windows のみ")
endif()

# MFC を静的リンク
set(CMAKE_MFC_FLAG 1)  # 1=静的, 2=動的

add_library(fitom_mfc_shim STATIC
    FITOMBridge.cpp
)

target_include_directories(fitom_mfc_shim
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(fitom_mfc_shim
    PUBLIC  fitom_core
)

target_compile_definitions(fitom_mfc_shim PRIVATE
    _AFXDLL      # MFC DLL 使用時は削除
    UNICODE
    _UNICODE
)
