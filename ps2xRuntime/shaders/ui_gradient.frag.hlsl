// Fragment shader of the UI layer for RmlUi's gradient decorators (linear-gradient, radial-gradient,
// conic-gradient and their repeating forms). It is a port of the fragment shader in RmlUi's own OpenGL 3
// renderer (RmlUi 6.3, Backends/RmlUi_Renderer_GL3.cpp, shader_frag_gradient, MIT licence, copyright the RmlUi
// Team and contributors): the gradient position t comes from the texture coordinate, which RmlUi fills with
// the position inside the element, and the colour is the stops mixed with smoothstep, times the vertex colour.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 0
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader (lines 2711-2741): fragment shaders use
// space3 for uniform buffers. Everything in the block is float4-sized so SPIR-V and DXIL lay it out the same.

#ifdef __spirv__
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif

#define MAX_NUM_STOPS 16
#define LINEAR 0
#define RADIAL 1
#define CONIC 2
#define REPEATING_LINEAR 3
#define REPEATING_RADIAL 4
#define REPEATING_CONIC 5
#define PI 3.14159265f

cbuffer Gradient : register(b0, space3)
{
    int4 Params;                      // x = function (LINEAR ...), y = number of stops
    float4 PV;                        // xy = p (start point, centre), zw = v (direction, curvature, unit vector)
    float4 StopColors[MAX_NUM_STOPS]; // premultiplied colours
    float4 StopPositions[4];          // 16 positions, 0 = start point, 1 = end point, packed four to a float4
};

struct PSInput
{
    LOCATION(0) float4 Color : TEXCOORD0;
    LOCATION(1) float2 UV : TEXCOORD1;
};

float stopPosition(int i)
{
    return StopPositions[i >> 2][i & 3];
}

float4 mixStopColors(float t, int numStops)
{
    float4 color = StopColors[0];
    for (int i = 1; i < numStops; i++)
    {
        const float a = stopPosition(i - 1);
        const float b = stopPosition(i);
        // smoothstep with a guard for equal edges, which GLSL leaves undefined: a hard step.
        const float s = saturate((t - a) / max(b - a, 1e-6f));
        color = lerp(color, StopColors[i], s * s * (3.0f - 2.0f * s));
    }
    return color;
}

float4 main(PSInput input) : SV_Target0
{
    const int func = Params.x;
    const int numStops = clamp(Params.y, 1, MAX_NUM_STOPS);
    const float2 p = PV.xy;
    const float2 v = PV.zw;
    float t = 0.0f;

    if (func == LINEAR || func == REPEATING_LINEAR)
    {
        const float distSquare = dot(v, v);
        t = dot(v, input.UV - p) / distSquare;
    }
    else if (func == RADIAL || func == REPEATING_RADIAL)
    {
        t = length(v * (input.UV - p));
    }
    else
    {
        // Conic: rotate by the unit vector (GLSL mat2(v.x, -v.y, v.y, v.x) times the offset), then the angle.
        const float2 d = input.UV - p;
        const float2 r = float2(d.x * v.x + d.y * v.y, d.y * v.x - d.x * v.y);
        t = 0.5f + atan2(-r.x, r.y) / (2.0f * PI);
    }

    if (func == REPEATING_LINEAR || func == REPEATING_RADIAL || func == REPEATING_CONIC)
    {
        const float t0 = stopPosition(0);
        const float t1 = stopPosition(numStops - 1);
        const float range = max(t1 - t0, 1e-6f);
        const float x = t - t0;
        t = t0 + (x - range * floor(x / range)); // GLSL mod(), not HLSL fmod()
    }

    return input.Color * mixStopColors(t, numStops);
}
