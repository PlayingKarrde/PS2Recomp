// Fragment shader of the GS GPU renderer for wrestler cards when the wrestlers are given shape (task lt3-T1; GSGpuBackend,
// src/lib/gs/gs_gpu_backend.cpp; docs/hd-lighting-tier3.md in the game's repository).
//
// A "card" is one flat part quad of a wrestler (or the referee, or a ring-side sprite). This is gs_lit.frag.hlsl with a third
// output. Targets 0 and 1 get exactly what the lit shader writes (LitPixel: the game's colour, and the light at the pixel by its
// position). Target 2, a half-float texture the size of the frame target, says for every pixel a card ends up owning (it passes the
// depth test like the colour does):
//   rgb  where the pixel is in the arena (world units);
//   a    which wrestler it belongs to and how much of the lighting it takes: the wrestler's mask channel (1 to 4, LightRule.w)
//        plus half of (the draw's share of the lighting x the pixel's blend coefficient). 0 means "not a card".
// The pass that shapes the wrestlers (postfx_rim.frag.hlsl) reads it at the end of the scene. Target 2 is not blended.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#define GS_LIT
#include "gs_frag_body.hlsli"
#include "gs_light.hlsli"
#include "gs_card.hlsli"

CardOutput main(PSInput input)
{
    return CardPixel(input);
}
