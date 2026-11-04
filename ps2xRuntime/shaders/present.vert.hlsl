// Vertex shader that puts the game's picture on the screen (the host's SDL_GPU presenter).
//
// It draws one rectangle, the 4:3 box, as two triangles with no vertex buffer: the corner comes from
// SV_VertexID. The rectangle is given in window pixels (origin top-left, y down), the way
// ps2_host_sdl.cpp's old SDL_Renderer path gave it to SDL_RenderTexture, and the texture coordinates
// for its corners come in the same uniform block. Everything outside the box stays at the clear colour.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): vertex shaders
// use space1 for uniform buffers. SV_VertexID is a system value, so the "TEXCOORD0.." rule for vertex
// attributes (lines 2754-2760) does not apply to it; the output below does use TEXCOORD0.
// SDL warns (SDL_gpu.h, SDL_DrawGPUPrimitives) that SV_VertexID is only reliable when the draw's
// first_vertex is 0, which it is.

#ifdef __spirv__
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif

cbuffer Place : register(b0, space1)
{
    float4 Rect;       // x0, y0, x1, y1 of the box in window pixels
    float4 UvRect;     // u0, v0, u1, v1 of the picture texture at the box's top-left and bottom-right corners
    float2 OutputSize; // window (swapchain) size in pixels
    float2 Pad;
};

struct VSOutput
{
    LOCATION(0) float2 UV : TEXCOORD0;
    float4 Position : SV_Position;
};

static const float2 kCorner[6] = {
    float2(0.0f, 0.0f), float2(1.0f, 0.0f), float2(1.0f, 1.0f),
    float2(0.0f, 0.0f), float2(1.0f, 1.0f), float2(0.0f, 1.0f)};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 corner = kCorner[vertexId];
    const float2 pixel = lerp(Rect.xy, Rect.zw, corner);
    VSOutput output;
    // Pixels to clip space: x -1..1 left to right, y 1..-1 top to bottom (SDL_GPU clip space is y-up
    // with the framebuffer origin at the top-left on every backend).
    const float2 ndc = pixel / OutputSize * 2.0f - 1.0f;
    output.Position = float4(ndc.x, -ndc.y, 0.0f, 1.0f);
    output.UV = lerp(UvRect.xy, UvRect.zw, corner);
    return output;
}
