// Smoke-test vertex shader for the SDL_GPU toolchain check (ps2x_gpu_smoke).
//
// It does what the GS renderer's vertex shader will do in its simplest form: take a position in
// target pixels, a depth in 0..1, a colour and a texture coordinate, and turn the position into
// clip space using the size of the render target.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): vertex
// shaders use space0 for sampled textures and storage, and space1 for uniform buffers.
// For SPIR-V dxc turns "spaceN" into descriptor set N, so the same source serves all formats.
// Vertex semantics start at TEXCOORD0 and count up (SDL_gpu.h lines 2754-2760): SDL's D3D12
// backend assumes every non system-value semantic is TEXCOORD, and the vertex attribute
// "location" in SDL_GPUVertexAttribute is the number after it.

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
    LOCATION(0) float2 Position : TEXCOORD0; // pixels, origin top-left, y down
    LOCATION(1) float Depth : TEXCOORD1;     // 0 (far) .. 1 (near)
    LOCATION(2) float4 Color : TEXCOORD2;    // RGBA8 unorm, 0.5 = 0x80
    LOCATION(3) float2 UV : TEXCOORD3;       // 0..1
};

struct VSOutput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float2 UV : TEXCOORD1;
    float4 Position : SV_Position;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    // Pixels to clip space: x -1..1 left to right, y 1..-1 top to bottom (SDL_GPU is y-up in clip
    // space with the framebuffer origin at the top-left, the same on every backend).
    float2 ndc = input.Position / TargetSize * 2.0f - 1.0f;
    output.Position = float4(ndc.x, -ndc.y, input.Depth, 1.0f);
    output.Color = input.Color;
    output.UV = input.UV;
    return output;
}
