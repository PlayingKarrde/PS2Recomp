# ps2x_ui_demo: the RmlUi overlay over a stand-in game picture, on SDL_GPU, windowed or headless.
#
# Included by ps2xTest/CMakeLists.txt when PS2X_BUILD_UI_DEMO is ON and the ps2_ui library exists
# (PS2X_BUILD_UI). It links ps2_ui, SDL and RmlUi, but not ps2_runtime and no generated game code.
# ps2_ui needs the shader loader ps2x::gpu::createShader, which lives in ps2_runtime; this program compiles that
# one file directly instead of linking the whole runtime.
add_executable(ps2x_ui_demo
    ui_demo_main.cpp
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/gpu/gpu_shaders.cpp
)
target_include_directories(ps2x_ui_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib
    ${CMAKE_SOURCE_DIR}/ps2xRuntime
)
target_link_libraries(ps2x_ui_demo PRIVATE ps2_ui)
target_compile_definitions(ps2x_ui_demo PRIVATE PS2X_UI_SOURCE_ASSET_DIR="${PS2X_UI_ASSET_DIR}")
ps2x_ui_stage_assets(ps2x_ui_demo)
if(UNIX AND NOT APPLE)
    target_link_libraries(ps2x_ui_demo PRIVATE ${CMAKE_DL_LIBS})
endif()
