# third_party/imgui_cmake/CMakeLists.txt
#
# Dear ImGui は公式にCMakeビルドシステムを同梱していないため、
# このラッパーをFITOM_X側で用意する。
# third_party/imgui (git submodule、素のまま・無改変) を相対パスで
# 参照する構成にすることで、submodule側のリポジトリにFITOM_X固有の
# ファイルを混入させないようにしている。
#
# バックエンドはGLFW(ウィンドウ/入力) + OpenGL3(描画)に固定する
# (apps/fitom_gui が前提とする組み合わせ)。

set(IMGUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui")

if(NOT EXISTS "${IMGUI_SOURCE_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "third_party/imgui が見つかりません。\n"
        "  git submodule update --init third_party/imgui")
endif()

add_library(imgui STATIC
    ${IMGUI_SOURCE_DIR}/imgui.cpp
    ${IMGUI_SOURCE_DIR}/imgui_demo.cpp
    ${IMGUI_SOURCE_DIR}/imgui_draw.cpp
    ${IMGUI_SOURCE_DIR}/imgui_tables.cpp
    ${IMGUI_SOURCE_DIR}/imgui_widgets.cpp
    ${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_SOURCE_DIR}
    ${IMGUI_SOURCE_DIR}/backends
)

# GLFW(third_party/glfwをadd_subdirectory済みである前提、トップレベル
# CMakeLists.txtで配線する)
target_link_libraries(imgui PUBLIC glfw)

if(WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC OpenGL::GL)
endif()
