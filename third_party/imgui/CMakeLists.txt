# third_party/imgui/CMakeLists.txt
# Dear ImGui は公式にCMakeビルドシステムを同梱していないため、
# このラッパーをFITOM_X側で用意する。
# バックエンドはGLFW(ウィンドウ/入力) + OpenGL3(描画)に固定する
# (apps/fitom_gui が前提とする組み合わせ)。

add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# GLFW(third_party/glfwをadd_subdirectory済みである前提、gui/bridgeと
# 同じレベルのトップレベルCMakeLists.txtで配線する)
target_link_libraries(imgui PUBLIC glfw)

if(WIN32)
    target_link_libraries(imgui PUBLIC opengl32)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC OpenGL::GL)
endif()
