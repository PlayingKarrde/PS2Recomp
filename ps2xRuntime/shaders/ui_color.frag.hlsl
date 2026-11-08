// Fragment shader of the UI layer for untextured geometry: the interpolated vertex colour, which RmlUi
// premultiplies by alpha. The blend (source one, destination one-minus-source-alpha) is fixed-function state
// in ui_render_sdlgpu.cpp.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 0
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): fragment shaders use
// space2 for textures and samplers and space3 for uniform buffers. Inputs match ui.vert.hlsl's outputs.

#ifdef __spirv__
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif

struct PSInput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float2 UV : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    return input.Color;
}
