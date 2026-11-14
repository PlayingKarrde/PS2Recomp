// Vertex shader of the GS GPU renderer for scene triangles that carry recovered world data (match lights, task lt2-L0b;
// GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp; gpu/gpu_lighting.h).
//
// It is gs.vert.hlsl plus a second vertex stream: for each vertex its world position multiplied by its Q, and the unit
// normal of its triangle (the same for all three corners). The renderer draws with a w of 1, so everything is interpolated
// straight across the screen; the world position is therefore sent times Q (which is 1 / w) and the fragment shader divides
// by the interpolated Q, exactly as the GS does for texture coordinates. That makes the position right at every pixel.
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
    LOCATION(3) float3 UVQ : TEXCOORD3;      // the GS S, T, Q
    LOCATION(4) float3 WorldQ : TEXCOORD4;   // world position x Q (second stream)
    LOCATION(5) float3 Normal : TEXCOORD5;   // the triangle's unit normal, turned to the camera (second stream)
};

struct VSOutput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float3 UVQ : TEXCOORD1;
    LOCATION(2) float3 WorldQ : TEXCOORD2;
    LOCATION(3) float3 Normal : TEXCOORD3;
    float4 Position : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float2 ndc = input.Position / TargetSize * 2.0f - 1.0f;
    output.Position = float4(ndc.x, -ndc.y, input.Depth, 1.0f);
    output.Color = input.Color;
    output.UVQ = input.UVQ;
    output.WorldQ = input.WorldQ;
    output.Normal = input.Normal;
    return output;
}
