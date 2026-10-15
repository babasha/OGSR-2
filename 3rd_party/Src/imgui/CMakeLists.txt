# Linux build of the vendored imgui (+ the two addons the engine uses). Source list copied
# from imgui.vcxproj — keep in sync. IMGUI_API is already empty in imconfig.h (static build).
cmake_minimum_required(VERSION 3.18)
project(imgui CXX)

add_library(imgui STATIC
    addons/ImGuiColorTextEdit/TextEditor.cpp
    addons/ImGuizmo/ImGuizmo.cpp
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(imgui PRIVATE NDEBUG)
