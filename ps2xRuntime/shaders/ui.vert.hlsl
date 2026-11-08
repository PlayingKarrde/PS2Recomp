// Vertex shader of the UI layer (src/lib/ui, the RmlUi overlay drawn through SDL_GPU).
//
// RmlUi hands the renderer geometry as vertices with a position in window pixels (origin top-left, y down),
// a premultiplied-alpha colour and a texture coordinate, and asks for it to be drawn translated by an offset
// and under a 4x4 matrix. The matrix includes the projection from window pixels to clip space (the renderer
// builds it, see ui_render_sdlgpu.cpp), so this shader only applies "Transform * (position + translation)".
// The matrix arrives in column-major order, the way Rml::Matrix4f stores it and the default for HLSL.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): vertex shaders use
// space1 for uniform buffers. Vertex semantics start at TEXCOORD0 and count up (lines 2754-2760).

#ifdef __spirv__
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif

cbuffer Place : register(b0, space1)
{
    float4x4 Transform; // projection * the element's transform
    float4 Translate;   // xy = offset added to the position before the matrix; zw unused
};

struct VSInput
{
    LOCATION(0) float2 Position : TEXCOORD0; // window pixels
    LOCATION(1) float4 Color : TEXCOORD1;    // RGBA8 unorm, premultiplied alpha
    LOCATION(2) float2 UV : TEXCOORD2;
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
    output.Color = input.Color;
    output.UV = input.UV;
    output.Position = mul(Transform, float4(input.Position + Translate.xy, 0.0f, 1.0f));
    return output;
}
