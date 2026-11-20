// Fragment shader of the GS GPU renderer for wrestler cards whose texture a texture pack replaces, when the wrestlers are given
// shape (task lt3-T1; GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp; docs/hd-lighting-tier3.md in the game's repository).
//
// It is gs_rep_lit.frag.hlsl with the third output of gs_card.frag.hlsl (see there and gs_card.hlsli).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#define GS_LIT
#include "gs_rep_frag_body.hlsli"
#include "gs_light.hlsli"
#include "gs_card.hlsli"

CardOutput main(PSInput input)
{
    return CardPixel(input);
}
