// Vertex shader of the GS GPU renderer (GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp).
//
// The backend does all the GS arithmetic that needs integers (12.4 fixed point positions, the
// OFX/OFY offset, sprite rectangles, flat shading) on the CPU and sends final positions in target
// pixels. This shader only turns pixels into clip space and passes the rest through. Depth arrives
// as Z / 2^24 (the GS Z buffer value, exact in a float); the target is a 32-bit float depth buffer,
// compared with GREATER_OR_EQUAL like the GS GEQUAL.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h (comment on SDL_CreateGPUShader): vertex uniforms in space1.
// Vertex semantics start at TEXCOORD0 and count up.

#ifdef __spirv__
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif

cbuffer Globals : register(b0, space1)
{
    float2 TargetSize; // render target size in pixels
    float2 Pad;
};

struct VSInput
{
    LOCATION(0) float2 Position : TEXCOORD0; // target pixels, origin top-left, y down
    LOCATION(1) float Depth : TEXCOORD1;     // GS Z / 2^24
    LOCATION(2) float4 Color : TEXCOORD2;    // RGBA8 unorm: the GS colour byte / 255
    LOCATION(3) float3 UVQ : TEXCOORD3;      // texel coordinates (u, v, 1), or the GS S, T, Q
};

struct VSOutput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float3 UVQ : TEXCOORD1;
    float4 Position : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float2 ndc = input.Position / TargetSize * 2.0f - 1.0f;
    output.Position = float4(ndc.x, -ndc.y, input.Depth, 1.0f);
    output.Color = input.Color;
    output.UVQ = input.UVQ;
    return output;
}
