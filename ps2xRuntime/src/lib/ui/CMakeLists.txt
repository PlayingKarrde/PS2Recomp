# The UI layer: an RmlUi overlay (settings pages and so on) drawn through SDL_GPU over the game picture.
#
# Added by ps2xRuntime/CMakeLists.txt when PS2X_BUILD_UI is ON (the default since the overlay is part of the game; OFF
# fetches nothing). ps2_runtime links it for linking only, so no compile command of the game changes. Nothing the
# generated game code includes refers to this folder.
#
# Third-party sources, both pinned and fetched from git like SDL (nothing is installed system-wide):
#   RmlUi    6.3          tag 6.3         commit ba95ffe8bfb6370efb2cdcca927eaad4710c5413 (2026-08-22), MIT
#   FreeType 2.14.3       tag VER-2-14-3  commit 0a0221a1347e2f1e07c395263540026e9a0aa7c7 (2026-03-22),
#                         FreeType License (BSD-style with an advertising clause) or GPL-2.0-only, our choice
# FreeType 2.15.0 (2026-10-11) was newer but under 30 days old when this was chosen; the same age rule as SDL.
# PS2X_RMLUI_SOURCE_DIR and PS2X_FREETYPE_SOURCE_DIR point a build tree at existing checkouts (no clone).

include(FetchContent)

set(PS2X_RMLUI_SOURCE_DIR "" CACHE PATH "Use this RmlUi checkout instead of cloning tag 6.3")
set(PS2X_FREETYPE_SOURCE_DIR "" CACHE PATH "Use this FreeType checkout instead of cloning tag VER-2-14-3")

# Both libraries are linked statically into whatever uses ps2_ui.
set(BUILD_SHARED_LIBS OFF)

# --- FreeType -------------------------------------------------------------------------------------------
# No optional libraries: FreeType would otherwise look for zlib, bzip2, libpng, HarfBuzz and Brotli on the
# machine, and a font loaded from memory or from a .ttf file does not need them.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

if(PS2X_FREETYPE_SOURCE_DIR)
    FetchContent_Declare(freetype SOURCE_DIR "${PS2X_FREETYPE_SOURCE_DIR}")
else()
    FetchContent_Declare(
        freetype
        GIT_REPOSITORY "https://github.com/freetype/freetype.git"
        GIT_TAG "VER-2-14-3"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
endif()
FetchContent_MakeAvailable(freetype)

# RmlUi's CMake/Dependencies.cmake calls find_package(Freetype) and then only checks that the target
# Freetype::Freetype exists, so give it ours first (FindFreetype does nothing when the target is there).
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# --- RmlUi ----------------------------------------------------------------------------------------------
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)

if(PS2X_RMLUI_SOURCE_DIR)
    FetchContent_Declare(rmlui SOURCE_DIR "${PS2X_RMLUI_SOURCE_DIR}")
else()
    FetchContent_Declare(
        rmlui
        GIT_REPOSITORY "https://github.com/mikke89/RmlUi.git"
        GIT_TAG "6.3"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
endif()
FetchContent_MakeAvailable(rmlui)

# --- ps2_ui ---------------------------------------------------------------------------------------------
# The compiled shaders include "gpu/gpu_shaders.h" (src/lib) and are found as "shaders/compiled/..." (relative to
# ps2xRuntime), like the GPU renderer's files. ps2_ui needs ps2x::gpu::createShader (src/lib/gpu/gpu_shaders.cpp),
# which ps2_runtime already holds; it is not compiled in a second time here, so the user of ps2_ui provides it:
# the game by linking ps2_runtime, ps2x_ui_demo by compiling that one file.
add_library(ps2_ui STATIC
    ui_game_menu.cpp
    ui_input.cpp
    ui_overlay.cpp
    ui_render_sdlgpu.cpp
    ui_settings.cpp
)
target_include_directories(ps2_ui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
)
target_link_libraries(ps2_ui PUBLIC ${PS2X_SDL3_TARGET} rmlui_core)
set_target_properties(ps2_ui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# The theme: RCSS, the pages and the fonts. They are loaded from a folder at run time (see ui_overlay.h).
# ps2x_ui_stage_assets(<target>) copies that folder to <folder of the target's program>/ui_assets before the
# target is built (every time, so an edited page or style sheet shows up without touching a source file).
set(PS2X_UI_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets" CACHE INTERNAL "Source of the UI theme, pages and fonts")
function(ps2x_ui_stage_assets target_name)
    add_custom_target(${target_name}_assets
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PS2X_UI_ASSET_DIR}" "$<TARGET_FILE_DIR:${target_name}>/ui_assets"
        COMMENT "Copying the UI assets next to ${target_name}"
        VERBATIM)
    add_dependencies(${target_name} ${target_name}_assets)
endfunction()
