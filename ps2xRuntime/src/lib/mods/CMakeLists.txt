# The mod layer: the package format and the mod manager (see mod_manager.h, docs/mod-layer.md, docs/formats/fprmod.md).
#
# Added by ps2xRuntime/CMakeLists.txt when PS2X_BUILD_MODS is ON (the default). ps2_runtime links it for linking only
# ($<LINK_ONLY:ps2_mods>), so no compile command of the game or of its generated code changes; nothing the generated code
# includes refers to this folder. The public headers hold no third-party types (toml11 and miniz stay inside the .cpp files).
#
# Third-party sources, both pinned and fetched from git like SDL (nothing is installed system-wide):
#   toml11  v4.4.0  commit be08ba2be2a964edcdb3d3e3ea8d100abc26f286 (2025-02-16), MIT
#           The same tag ps2xRecomp/CMakeLists.txt pins for the recompiler's configuration files. FetchContent keeps the
#           first declaration of a name, so when the recompiler is part of the build its fetch is the one used.
#   miniz   3.1.2   tag 3.1.2  commit 77d0dce8627735138c51770d1799a1ef48f2117d (2026-07-01), MIT
#           Reads and (for the tests) writes the zip files that mods are. 3.1.2 was released more than 30 days before it was
#           chosen, the same age rule as SDL and FreeType.
# PS2X_MINIZ_SOURCE_DIR points a build tree at an existing miniz checkout (no clone).

include(FetchContent)

set(PS2X_MINIZ_SOURCE_DIR "" CACHE PATH "Use this miniz checkout instead of cloning tag 3.1.2")

FetchContent_Declare(
    toml11
    GIT_REPOSITORY https://github.com/ToruNiina/toml11.git
    GIT_TAG v4.4.0
)
FetchContent_MakeAvailable(toml11)

# miniz is a C library built as a static library; its own CMake adds no examples, tests or install rules when it is not the
# top-level project.
set(BUILD_SHARED_LIBS OFF)
if(PS2X_MINIZ_SOURCE_DIR)
    FetchContent_Declare(miniz SOURCE_DIR "${PS2X_MINIZ_SOURCE_DIR}")
else()
    FetchContent_Declare(
        miniz
        GIT_REPOSITORY "https://github.com/richgel999/miniz.git"
        GIT_TAG "3.1.2"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
endif()
FetchContent_MakeAvailable(miniz)

add_library(ps2_mods STATIC
    mod_manifest.cpp
    mod_package.cpp
    mod_state.cpp
    mod_manager.cpp
    mini_json.cpp
    fpr_save.cpp
    data_pack.cpp
    profile.cpp
)
target_include_directories(ps2_mods PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ps2_mods PRIVATE toml11::toml11 miniz)
set_target_properties(ps2_mods PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
