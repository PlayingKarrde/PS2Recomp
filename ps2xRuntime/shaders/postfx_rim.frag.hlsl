// Wrestler shape: the outline of the whole wrestler and a rim of light (task lt3-T1, gpu_postfx.cpp; docs/hd-lighting-tier3.md
// in the game's repository; the C++ reference of the arithmetic is ps2x_lighting::wrestlerShapeAt in gpu/gpu_lighting.cpp).
//
// A wrestler is flat pictures, so the lit shader can only tint him by where he stands. This pass gives him a little shape from
// his outline on the screen: near the outline the surface is taken to lean outward, as it would on a rounded body, and the lights
// are worked out again for that leaning surface. It runs once per field, at the end of the scene, and writes a copy of the
// frame's light buffer in which the pixels of the wrestlers carry the new light; the multiply pass (postfx_light.frag.hlsl) then
// reads that copy. Every other pixel is copied as it is.
//
// Inputs (all the size of the frame target except Blur):
//   Light   the light buffer of the lit draws (half-float, linear light)
//   Where   written by the wrestler cards (gs_card.hlsli): rgb = the pixel's place in the arena, a = mask channel (1 to 4) plus
//           half the share of the lighting the pixel takes; 0 where the pixel is not a card
//   Blur    the blurred wrestler mask (postfx_rim_blur.frag.hlsl), the size of the picture, one channel per wrestler
// Parameters (the block of every pass, read in this pass's own way):
//   Src.xy   where the picture starts in the frame target
//   Src.zw   the picture's size
//   Aux.xyz  the camera's position (world units); Aux.w: +1 or -1, whether cross(up, view) points to the right of the screen
//   Tilt.x   shape strength (0 = today's tint by position, 1 = the full effect)
//   Tilt.y   rim strength
//   Tilt.w   how far the surface leans at the very outline (0.92)
// and the lights of the look, in the block of the lit shaders (gs_light.hlsli; the two declarations must stay the same).
//
// The arithmetic, per wrestler pixel (route (d) of the study):
//   b       the blurred mask of the pixel's own wrestler; lean = saturate((1 - b) x 2) x 0.92, towards where b falls
//   N       the surface direction: the camera's view of a flat-on surface, tipped by `lean` in the screen's plane
//   shaped  ambient + sum over the lights of (light x wrap(N.L)) x gain, wrap(x) = saturate((x + 1/2) / (3/2)); the gain makes a
//           flat-on pixel under the look's first light exactly as bright as it is today
//   rim     sum over the lights of (light x max(N2.L, 0)) x lean^2, N2 = the lean's direction lying in the screen's plane
//   result  today's light + shape strength x (shaped - today's light) + rim strength x rim, times the pixel's share
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 3
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#include "postfx_common.hlsli"

#define MAX_LIGHTS 8

cbuffer Light : register(b1, space3)
{
    float4 LightAmbient;                // rgb; w: how many lights follow
    float4 LightRule;                   // not used here
    float4 LightPosition[MAX_LIGHTS];   // xyz: where the light is (world units), w: its range (no light beyond it)
    float4 LightAxis[MAX_LIGHTS];       // xyz: the unit direction a spotlight points in, w: its intensity
    float4 LightColour[MAX_LIGHTS];     // rgb, w: 1 a spotlight, 0 a point light
    float4 LightCone[MAX_LIGHTS];       // x: cos(outer half angle), y: cos(inner half angle)
};

COMBINED Texture2D<float4> Light : register(t0, space2);
COMBINED SamplerState LightSamp : register(s0, space2);
COMBINED Texture2D<float4> Where : register(t1, space2);
COMBINED SamplerState WhereSamp : register(s1, space2);
COMBINED Texture2D<float4> Blur : register(t2, space2);
COMBINED SamplerState BlurSamp : register(s2, space2);

#define WRAP 0.5f

float BlurAt(int2 p, float4 channel)
{
    p = clamp(p, int2(0, 0), int2(Src.zw) - 1);
    return dot(Blur.Load(int3(p, 0)), channel);
}

float4 main(PSInput input) : SV_Target0
{
    const int2 q = int2(input.Position.xy); // this pixel in the frame target
    const float4 light = Light.Load(int3(q, 0));
    const float4 where = Where.Load(int3(q, 0));
    if (where.a < 0.75f)
        return light; // not a wrestler's pixel
    const float group = floor(where.a);
    const float share = (where.a - group) * 2.0f;
    const float4 channel = float4(group == 1.0f ? 1.0f : 0.0f, group == 2.0f ? 1.0f : 0.0f, group == 3.0f ? 1.0f : 0.0f, group >= 4.0f ? 1.0f : 0.0f);

    // The outline: how far down the blurred mask this pixel is, and which way the mask falls (y grows down the screen).
    const int2 p = q - int2(Src.xy);
    const float b = BlurAt(p, channel);
    const float gx = (BlurAt(p + int2(1, 0), channel) - BlurAt(p - int2(1, 0), channel)) * 0.5f;
    const float gy = (BlurAt(p + int2(0, 1), channel) - BlurAt(p - int2(0, 1), channel)) * 0.5f;
    const float steep = sqrt(gx * gx + gy * gy);
    const float lean = saturate((1.0f - b) * 2.0f) * Tilt.w;
    const float2 out2 = steep > 1.0e-7f ? float2(-gx, -gy) / steep : float2(0.0f, 0.0f); // the way out, on the screen

    // The camera's axes at this pixel, in the world: V away from the eye, R to the right of the screen, U up the screen.
    const float3 world = where.rgb;
    const float3 V = normalize(world - Aux.xyz);
    float3 R = cross(float3(0.0f, 1.0f, 0.0f), V);
    const float rl = length(R);
    R = rl > 1.0e-5f ? R / rl : float3(1.0f, 0.0f, 0.0f);
    const float3 U = cross(V, R);
    R *= Aux.w;
    const float2 s = out2 * lean;
    const float3 N = normalize(s.x * R - s.y * U - sqrt(max(1.0f - lean * lean, 0.02f)) * V);
    const float3 N2 = normalize(s.x * R - s.y * U - 0.001f * V);

    float3 today = float3(0.0f, 0.0f, 0.0f), shaped = float3(0.0f, 0.0f, 0.0f), rim = float3(0.0f, 0.0f, 0.0f);
    float gain = 1.0f;
    const int count = (int)LightAmbient.w;
    for (int k = 0; k < MAX_LIGHTS; ++k)
    {
        if (k >= count)
            break;
        const float3 toLight = LightPosition[k].xyz - world;
        const float distance = length(toLight);
        const float3 l = distance > 0.0f ? toLight / distance : float3(0.0f, 1.0f, 0.0f);
        if (k == 0)
            gain = 1.0f / clamp((dot(-V, l) + WRAP) / (1.0f + WRAP), 0.3f, 1.0f);
        if (distance >= LightPosition[k].w)
            continue;
        const float ratio = distance / LightPosition[k].w;
        const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
        float cone = 1.0f;
        if (LightColour[k].w > 0.5f)
            cone = smoothstep(LightCone[k].x, LightCone[k].y, -dot(LightAxis[k].xyz, l));
        const float3 c = LightColour[k].rgb * (LightAxis[k].w * fall * cone);
        today += c;
        shaped += c * saturate((dot(N, l) + WRAP) / (1.0f + WRAP));
        rim += c * max(dot(N2, l), 0.0f);
    }
    const float3 change = Tilt.x * (shaped * gain - today) + Tilt.y * (lean * lean) * rim;
    return float4(max(light.rgb + change * share, 0.0f), light.a);
}
