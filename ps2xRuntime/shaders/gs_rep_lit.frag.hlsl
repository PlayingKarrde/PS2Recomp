// Fragment shader of the GS GPU renderer for scene triangles that are lit (match lights, task lt2-L1; GSGpuBackend,
// src/lib/gs/gs_gpu_backend.cpp; docs/hd-lighting-tier2.md in the game's repository).
//
// It is gs_rep.frag.hlsl (a texture that a texture pack replaces) with a second output. Target 0 gets the game's colour, from the same code (gs_rep_frag_body.hlsli), so
// the game's frame is what it would be without lights. Target 1, a half-float texture the size of the frame target, gets the light
// that arrives at the pixel (gs_light.hlsli): ambient plus the spotlight, from the world position and normal the renderer recovered
// for the triangle. The screen effects multiply the finished scene by that texture (postfx_light.frag.hlsl).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#define GS_LIT
#include "gs_rep_frag_body.hlsli"
#include "gs_light.hlsli"

LitOutput main(PSInput input)
{
    return LitPixel(input);
}
