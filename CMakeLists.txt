cmake_minimum_required(VERSION 3.20)
project(RigEditor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- GLFW ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)

# --- Dear ImGui (docking branch) ---
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b-docking
  GIT_SHALLOW TRUE)

# --- stb (stb_image) ---
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20)

# --- nlohmann/json (single header) ---
set(JSON_HPP_DIR ${CMAKE_BINARY_DIR}/_deps/nlohmann)
if(NOT EXISTS ${JSON_HPP_DIR}/json.hpp)
  file(DOWNLOAD
    https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    ${JSON_HPP_DIR}/json.hpp
    EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
    SHOW_PROGRESS)
endif()

# --- miniaudio (single header, public domain / MIT-0) ---
set(MINIAUDIO_DIR ${CMAKE_BINARY_DIR}/_deps/miniaudio)
if(NOT EXISTS ${MINIAUDIO_DIR}/miniaudio.h)
  file(DOWNLOAD
    https://raw.githubusercontent.com/mackron/miniaudio/0.11.22/miniaudio.h
    ${MINIAUDIO_DIR}/miniaudio.h
    EXPECTED_HASH SHA256=9019743287e443c55e5737a7297f38e5e358561701d6db2d905afb114390c410
    SHOW_PROGRESS)
endif()

FetchContent_MakeAvailable(glfw imgui stb)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# --- core: pure data + math, no UI dependency (reusable as a runtime) ---
add_library(rigcore STATIC
  src/core/model.cpp
  src/core/pose.cpp
  src/core/anim_state.cpp
  src/core/mesh_tools.cpp
  src/core/mesh_edit.cpp
  src/core/clipping.cpp
  src/core/constraints.cpp
  src/core/atlas.cpp
  src/core/raster.cpp
  src/core/merge.cpp
  src/core/psd.cpp
  src/core/audio_mix.cpp
  src/core/serialize.cpp)
target_include_directories(rigcore PUBLIC src ${JSON_HPP_DIR})

add_executable(RigEditor
  src/main.cpp
  src/platform/texture.cpp
  src/platform/file_dialog.cpp
  src/platform/audio.cpp
  src/editor/editor.cpp
  src/editor/theme.cpp
  src/editor/icons.cpp
  src/editor/panel_viewport.cpp
  src/editor/viewport_toolbar.cpp
  src/editor/panel_tree.cpp
  src/editor/panel_properties.cpp
  src/editor/panel_dopesheet.cpp
  src/editor/panel_graph.cpp
  src/editor/panel_mix.cpp
  src/editor/panel_io.cpp)
target_include_directories(RigEditor PRIVATE src ${stb_SOURCE_DIR} ${MINIAUDIO_DIR})
target_link_libraries(RigEditor PRIVATE rigcore imgui)
if(WIN32)
  target_link_libraries(RigEditor PRIVATE comdlg32)
  target_compile_definitions(RigEditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
  # Third-party single-header implementation: keep its warnings out of the build log.
  set_source_files_properties(src/platform/audio.cpp PROPERTIES COMPILE_OPTIONS "/W0")
  target_compile_options(rigcore PRIVATE /W4 /utf-8)
  target_compile_options(RigEditor PRIVATE /W4 /utf-8)
  set_property(TARGET RigEditor PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# --- tests for core ---
enable_testing()
add_executable(rigcore_tests tests/core_tests.cpp)
target_link_libraries(rigcore_tests PRIVATE rigcore)
if(MSVC)
  target_compile_options(rigcore_tests PRIVATE /utf-8)
endif()
add_test(NAME rigcore_tests COMMAND rigcore_tests)
