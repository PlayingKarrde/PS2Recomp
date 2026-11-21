// Fragment shader of the GS GPU renderer for wrestler cards whose texture a texture pack replaces, when the wrestlers get shape from
// generated normal maps as well (task lt3-T3; GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp; docs/hd-lighting-tier3.md in the game's
// repository).
//
// It is gs_rep_card.frag.hlsl with the fourth output of gs_card_n.frag.hlsl (see there and gs_card.hlsli). The map is made from the
// original tile, not from the replacement picture, and is the third texture (slot 2).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 3
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#define GS_LIT
#define CARD_NORMALS
#define CARD_NORMAL_T t2
#define CARD_NORMAL_S s2
#include "gs_rep_frag_body.hlsli"
#include "gs_light.hlsli"
#include "gs_card.hlsli"

CardOutput main(PSInput input)
{
    return CardPixel(input);
}
