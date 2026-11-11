cmake_minimum_required(VERSION 3.21)

project(ps2xTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(gs_cache)
endif()

# Static library with test logic (no main), used by ps2xStudio
add_library(ps2_test_function_table OBJECT
    src/test_function_table.cpp
)

target_include_directories(ps2_test_function_table PRIVATE
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xIOP/include
)

add_library(ps2_test_lib STATIC
    src/code_generator_tests.cpp
    src/r5900_decoder_tests.cpp
    src/elf_analyzer_tests.cpp
    src/pad_input_tests.cpp
    src/ps2_runtime_io_tests.cpp
    src/ps2_runtime_kernel_tests.cpp
    src/ps2_runtime_interrupt_tests.cpp
    src/ps2_memory_tests.cpp
    src/ps2_vu1_tests.cpp
    src/ps2_vu_tests.cpp
    src/ps2_gs_tests.cpp
    src/ps2_iop_tests.cpp
    src/ps2_sif_rpc_tests.cpp
    src/ps2_sif_dma_tests.cpp
    src/ps2_recompiler_tests.cpp
    src/ps2_runtime_expansion_tests.cpp
    src/ps2_float_semantics_tests.cpp
    src/gs_recording_tests.cpp
    src/gs_gpu_tests.cpp
    src/gs_tex_pack_tests.cpp
    src/gs_font_tests.cpp
    src/ui_settings_tests.cpp
)

# The mod layer's tests (src/lib/mods) exist when the build has the mod manager (PS2X_BUILD_MODS). The tests use only the
# manager's own headers and the static library; the game build does not include ps2xTest.
if(TARGET ps2_mods)
    target_sources(ps2_test_lib PRIVATE src/mod_manager_tests.cpp src/data_pack_tests.cpp src/save_container_tests.cpp src/save_import_tests.cpp
                   src/edit_text_tests.cpp src/edit_pack_tests.cpp)
    target_link_libraries(ps2_test_lib PRIVATE ps2_mods)
    target_compile_definitions(ps2_test_lib PRIVATE PS2X_HAS_MODS_TESTS=1)
endif()

target_include_directories(ps2_test_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib
)

target_link_libraries(ps2_test_lib PRIVATE
    ps2_recomp_lib
    ps2_analyzer_lib
    ps2_runtime
    ps2_iop
)

if(UNIX AND NOT APPLE)
    target_link_libraries(ps2_test_lib PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(ps2x_tests
    src/main.cpp
    $<TARGET_OBJECTS:ps2_test_function_table>
)

option(PRINT_GENERATED_CODE "Print generated code in tests" OFF)
if(PRINT_GENERATED_CODE)

    target_compile_definitions(ps2x_tests PRIVATE PRINT_GENERATED_CODE)
    target_compile_definitions(ps2_test_lib PRIVATE PRINT_GENERATED_CODE)
endif()

if(TARGET ps2_mods)
    target_compile_definitions(ps2x_tests PRIVATE PS2X_HAS_MODS_TESTS=1)
endif()

target_include_directories(ps2x_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/ps2xRecomp/include
    ${CMAKE_SOURCE_DIR}/ps2xAnalyzer/include
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
)

target_link_libraries(ps2x_tests PRIVATE
    ps2_test_lib
    ps2_recomp_lib
    ps2_analyzer_lib
    ps2_runtime
)

# VU/GS tests intentionally instantiate large interpreter state.
# Windows executables default to a much smaller stack than Linux, and
# sanitizer/debug instrumentation can push these test frames over 1 MiB.
if(MSVC)
    target_link_options(ps2x_tests PRIVATE "/STACK:8388608")
elseif(MINGW)
    target_link_options(ps2x_tests PRIVATE "--stack,8388608")
endif()

if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2x_tests)
endif()

include("${CMAKE_SOURCE_DIR}/ps2xRuntime/cmake/ReleaseMode.cmake")

# ps2x_gs_replay plays back a GS recording (PS2X_GS_RECORD, see ps2xRuntime/include/runtime/gs/gs_recording.h)
# through the CPU renderer, without the game. It links the runtime library but no generated game code,
# and nothing in the game's build refers to it (the game is configured with PS2X_BUILD_TEST=OFF).
option(PS2X_BUILD_GS_REPLAY "Build ps2x_gs_replay, the standalone GS recording player" ON)
if(PS2X_BUILD_GS_REPLAY)
    add_executable(ps2x_gs_replay
        gs_replay/gs_replay_main.cpp
        $<TARGET_OBJECTS:ps2_test_function_table>
    )
    target_include_directories(ps2x_gs_replay PRIVATE
        ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
    )
    target_link_libraries(ps2x_gs_replay PRIVATE ps2_runtime)
    if(UNIX AND NOT APPLE)
        target_link_libraries(ps2x_gs_replay PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        ps2x_stage_ffmpeg_runtime_dlls(ps2x_gs_replay)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        EnableFastReleaseMode(ps2x_gs_replay)
    endif()
endif()

# ps2x_gpu_smoke checks the SDL_GPU shader toolchain: the HLSL shaders in ps2xRuntime/shaders, compiled
# offline into the committed headers in shaders/compiled/, are loaded by SDL_GPU and draw a test scene.
# It links the runtime library for the shader loader (src/lib/gpu/gpu_shaders.cpp) and SDL, but no generated
# game code, and nothing in the game's build refers to it (the game is configured with PS2X_BUILD_TEST=OFF).
option(PS2X_BUILD_GPU_SMOKE "Build ps2x_gpu_smoke, the SDL_GPU shader toolchain smoke test" ON)
if(PS2X_BUILD_GPU_SMOKE)
    add_executable(ps2x_gpu_smoke
        gpu_smoke/gpu_smoke_main.cpp
        $<TARGET_OBJECTS:ps2_test_function_table>
    )
    target_include_directories(ps2x_gpu_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/ps2xRuntime/include
        # "gpu/gpu_shaders.h" lives in src/lib; "shaders/compiled/*.h" is relative to ps2xRuntime.
        # These are added here, not to ps2_runtime, so the game's compile commands do not change.
        ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib
        ${CMAKE_SOURCE_DIR}/ps2xRuntime
    )
    target_link_libraries(ps2x_gpu_smoke PRIVATE ps2_runtime)
    if(UNIX AND NOT APPLE)
        target_link_libraries(ps2x_gpu_smoke PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
        ps2x_stage_ffmpeg_runtime_dlls(ps2x_gpu_smoke)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        EnableFastReleaseMode(ps2x_gpu_smoke)
    endif()
endif()

# ps2x_ui_demo shows the RmlUi overlay (ps2xRuntime/src/lib/ui) over a stand-in game picture, windowed or headless.
# Only when PS2X_BUILD_UI is ON (it needs the ps2_ui library); no generated game code is involved.
option(PS2X_BUILD_UI_DEMO "Build ps2x_ui_demo, the standalone RmlUi overlay demo" ON)
if(PS2X_BUILD_UI_DEMO AND TARGET ps2_ui)
    add_subdirectory(ui_demo)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_test_lib)
    EnableFastReleaseMode(ps2x_tests)
endif()
