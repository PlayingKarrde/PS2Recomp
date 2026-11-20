// Fragment shader of the GS GPU renderer for the wrestler mask, for a card whose texture a texture pack replaces (task lt3-T1;
// GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp). It is gs_mask.frag.hlsl over the body of gs_rep.frag.hlsl, so the mask has the
// cut-out of the replacement picture.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#include "gs_rep_frag_body.hlsli"

cbuffer Mask : register(b1, space3)
{
    float4 MaskChannel; // 1 in the wrestler's channel, 0 in the others
};

float4 main(PSInput input) : SV_Target0
{
    GsColour(input); // for its alpha test: a texel that is cut out is discarded
    return MaskChannel;
}
