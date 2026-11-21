// Fragment shader of the GS GPU renderer for wrestler cards when the wrestlers get shape from generated normal maps as well (task
// lt3-T3; GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp; docs/hd-lighting-tier3.md in the game's repository).
//
// It is gs_card.frag.hlsl with a fourth output: what the tile's generated normal map says about the pixel, turned to the screen
// (gs_card.hlsli, CARD_NORMALS). Targets 0 to 2 get exactly what gs_card.frag.hlsl writes. Target 3 is an 8-bit texture of the
// frame target's size and is not blended. The map is the second texture (slot 1).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#define GS_LIT
#define CARD_NORMALS
#define CARD_NORMAL_T t1
#define CARD_NORMAL_S s1
#include "gs_frag_body.hlsli"
#include "gs_light.hlsli"
#include "gs_card.hlsli"

CardOutput main(PSInput input)
{
    return CardPixel(input);
}
