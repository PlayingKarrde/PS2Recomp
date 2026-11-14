// Vertex shader of the screen effects (task fx1, gpu_postfx.cpp): one triangle that covers the whole render target, made from
// SV_VertexID with no vertex buffer. Every pass of the effect chain uses it; the pass's own fragment shader (postfx_*.frag.hlsl)
// reads the pixel it is drawing from SV_Position, which is in texels of the target being drawn (origin top-left).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 0
//
// Register spaces follow SDL_gpu.h; see present.vert.hlsl. The output carries a TEXCOORD0 first and SV_Position after it so
// that Direct3D 12 matches the registers of the fragment shaders' input (see field_select.frag.hlsl).

#ifdef __spirv__
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif

struct VSOutput
{
    LOCATION(0) float2 UV : TEXCOORD0;
    float4 Position : SV_Position;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    // 0: (-1,-1)  1: (3,-1)  2: (-1,3): one triangle that covers the clip square.
    const float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.UV = corner;
    output.Position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}
