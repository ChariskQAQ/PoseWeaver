cmake_minimum_required(VERSION 3.20)
project(Poseweaver VERSION 1.1.1 LANGUAGES C CXX)

# Static C/C++ runtime: the packaged exe runs without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE BOOL "" FORCE)  # GLFW's own switch

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
find_package(Threads REQUIRED)

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
  src/core/render_effects.cpp
  src/core/merge.cpp
  src/core/psd.cpp
  src/core/audio_mix.cpp
  src/core/file_io.cpp
  src/core/diagnostics.cpp
  src/core/project_resources.cpp
  src/core/resource_relink.cpp
  src/core/serialize.cpp)
target_include_directories(rigcore PUBLIC src ${JSON_HPP_DIR})
set_target_properties(rigcore PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(poseweaver_runtime SHARED src/runtime/runtime.cpp src/runtime/godot_bridge.cpp)
target_link_libraries(poseweaver_runtime PRIVATE rigcore)
target_include_directories(poseweaver_runtime PRIVATE third_party/godot)
target_compile_definitions(poseweaver_runtime PRIVATE POSEWEAVER_RUNTIME_BUILD NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(poseweaver_runtime PRIVATE /W4 /utf-8)
endif()

add_library(rigeditor STATIC
  src/platform/blend.cpp
  src/platform/gpu_effects.cpp
  src/platform/texture.cpp
  src/platform/image_write.cpp
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
  src/editor/splash.cpp
  src/editor/project_safety.cpp
  src/editor/panel_issues.cpp
  src/editor/panel_resource_relink.cpp
  src/editor/panel_export_recovery.cpp
  src/editor/i18n.cpp
  src/editor/engine_export.cpp
  src/editor/runtime_export.cpp
  src/editor/image_export.cpp
  src/editor/export_transaction.cpp
  src/editor/image_settings.cpp
  src/editor/panel_engine_export.cpp
  src/editor/panel_effects.cpp
  src/editor/panel_io.cpp)
target_include_directories(rigeditor PUBLIC src PRIVATE ${stb_SOURCE_DIR} ${MINIAUDIO_DIR} ${glfw_SOURCE_DIR}/deps)
target_compile_definitions(rigeditor PRIVATE POSEWEAVER_VERSION="${PROJECT_VERSION}")
target_link_libraries(rigeditor PUBLIC rigcore imgui Threads::Threads)
add_executable(Poseweaver src/main.cpp)
target_include_directories(Poseweaver PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(Poseweaver PRIVATE rigeditor)
if(WIN32)
  target_link_libraries(rigeditor PUBLIC comdlg32 ole32 uuid)
  target_compile_definitions(rigeditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_definitions(Poseweaver PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  configure_file(assets/app.rc.in ${CMAKE_BINARY_DIR}/app.rc @ONLY)
  target_sources(Poseweaver PRIVATE ${CMAKE_BINARY_DIR}/app.rc)
endif()
if(MSVC)
  # Third-party single-header implementation: keep its warnings out of the build log.
  set_source_files_properties(src/platform/audio.cpp PROPERTIES COMPILE_OPTIONS "/W0")
  target_compile_options(rigcore PRIVATE /W4 /utf-8)
  target_compile_options(rigeditor PRIVATE /W4 /utf-8)
  target_compile_options(Poseweaver PRIVATE /W4 /utf-8)
  # A GUI program (no console window) that still starts at main().
  target_link_options(Poseweaver PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
  set_property(TARGET Poseweaver PROPERTY VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# --- tests for core ---
enable_testing()
add_executable(rigcore_tests tests/core_tests.cpp)
add_executable(make_robot_sample scripts/make_robot_sample.cpp)
target_link_libraries(make_robot_sample PRIVATE rigcore)
target_link_libraries(rigcore_tests PRIVATE rigcore)
if(MSVC)
  target_compile_options(rigcore_tests PRIVATE /utf-8)
endif()
add_test(NAME rigcore_tests COMMAND rigcore_tests)
add_executable(runtime_tests tests/runtime_tests.cpp)
target_link_libraries(runtime_tests PRIVATE rigcore poseweaver_runtime)
target_compile_definitions(runtime_tests PRIVATE POSEWEAVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(runtime_tests PRIVATE /utf-8)
endif()
add_test(NAME runtime_tests COMMAND runtime_tests)

add_executable(project_safety_tests tests/project_safety_tests.cpp)
target_link_libraries(project_safety_tests PRIVATE rigcore)
add_test(NAME project_safety_tests COMMAND project_safety_tests)

add_executable(editor_flow_tests tests/editor_flow_tests.cpp)
target_link_libraries(editor_flow_tests PRIVATE rigeditor)
target_compile_definitions(editor_flow_tests PRIVATE POSEWEAVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
  target_compile_options(project_safety_tests PRIVATE /utf-8)
  target_compile_options(editor_flow_tests PRIVATE /utf-8)
endif()
add_test(NAME editor_flow_tests COMMAND editor_flow_tests)
add_executable(image_export_tests tests/image_export_tests.cpp)
add_executable(encoder_block_fixture tests/encoder_block_fixture.cpp)
add_dependencies(image_export_tests encoder_block_fixture)
target_compile_definitions(image_export_tests PRIVATE ENCODER_BLOCK_FIXTURE="$<TARGET_FILE:encoder_block_fixture>")
target_link_libraries(image_export_tests PRIVATE rigeditor)
if(MSVC)
  target_compile_options(image_export_tests PRIVATE /utf-8)
endif()
add_test(NAME image_export_tests COMMAND image_export_tests)
add_executable(export_recovery_tests tests/export_recovery_tests.cpp)
target_link_libraries(export_recovery_tests PRIVATE rigeditor)
if(WIN32)
  target_compile_definitions(export_recovery_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_test(NAME export_recovery_tests COMMAND export_recovery_tests)
set_tests_properties(export_recovery_tests PROPERTIES TIMEOUT 45)
set_tests_properties(image_export_tests PROPERTIES TIMEOUT 45)
add_test(NAME engine_export_cli COMMAND ${CMAKE_COMMAND}
  "-DPROGRAM=$<TARGET_FILE:Poseweaver>" "-DROOT=${CMAKE_BINARY_DIR}"
  "-DSAMPLE=${CMAKE_SOURCE_DIR}/samples/arm.png" -P "${CMAKE_SOURCE_DIR}/tests/engine_export_cli.cmake")
set_tests_properties(engine_export_cli PROPERTIES TIMEOUT 30)

add_executable(blend_render_tests tests/blend_render_tests.cpp)
target_link_libraries(blend_render_tests PRIVATE rigeditor)
add_test(NAME blend_render_tests COMMAND blend_render_tests)

add_executable(gpu_effects_tests tests/gpu_effects_tests.cpp)
target_include_directories(gpu_effects_tests PRIVATE ${glfw_SOURCE_DIR}/deps)
target_link_libraries(gpu_effects_tests PRIVATE rigeditor)
add_test(NAME gpu_effects_tests COMMAND gpu_effects_tests)
set_tests_properties(gpu_effects_tests PROPERTIES TIMEOUT 45)

add_executable(rigcore_bench tests/performance_bench.cpp)
add_executable(rigeditor_bench tests/editor_bench.cpp)
target_include_directories(rigeditor_bench PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(rigeditor_bench PRIVATE rigeditor)
if(WIN32)
  target_compile_definitions(rigeditor_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
add_test(NAME editor_performance_smoke COMMAND rigeditor_bench --smoke)
set_tests_properties(editor_performance_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
target_link_libraries(rigcore_bench PRIVATE rigcore)
add_test(NAME performance_fixture_smoke COMMAND rigcore_bench --smoke)

add_executable(riggpu_bench tests/gpu_bench.cpp)
target_include_directories(riggpu_bench PRIVATE ${glfw_SOURCE_DIR}/deps)
target_link_libraries(riggpu_bench PRIVATE rigeditor)
add_test(NAME gpu_performance_smoke COMMAND riggpu_bench --smoke)
set_tests_properties(gpu_performance_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
