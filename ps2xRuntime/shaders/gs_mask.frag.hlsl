// Fragment shader of the GS GPU renderer for the wrestler mask (task lt3-T1; GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp;
// docs/hd-lighting-tier3.md in the game's repository).
//
// Every wrestler card is drawn a second time into the mask, a texture of the frame target's size, with NO depth test: the mask is
// the whole outline of the wrestler, also where a rope or a post hides him (otherwise whatever stands in front would cut a false
// edge into the outline). The texturing and the alpha test are the plain shader's (gs_frag_body.hlsli), so the mask has the
// card's own cut-out. The output is 1 in the wrestler's channel (MaskChannel: wrestlers that stand in different depth planes get
// different channels, so one in front of another keeps his own outline) and the blend keeps the larger value per channel.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#include "gs_frag_body.hlsli"

cbuffer Mask : register(b1, space3)
{
    float4 MaskChannel; // 1 in the wrestler's channel, 0 in the others
};

float4 main(PSInput input) : SV_Target0
{
    GsColour(input); // for its alpha test: a texel that is cut out is discarded
    return MaskChannel;
}
