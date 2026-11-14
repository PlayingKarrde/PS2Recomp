// The match lights applied to the scene (task lt2-L1, gpu_postfx.cpp; docs/hd-lighting-tier2.md in the game's repository).
//
// Inputs: Field is the frame target at the end of the scene (8 bits, display encoding: the game's own colours, untouched by the
// lights), Light is the light buffer the lit draws wrote beside it (half-float, linear light, the size of the frame target; 1.0
// wherever nothing lit was drawn). The pass multiplies the two in linear light.
//
// A pixel whose light is exactly 1.0 is passed through as the bytes it had: an unlit pixel comes out as it does without lights.
//
// Two forms (Mode.x):
//   0  the first pass of the chain. The result goes into a half-float texture the size of the picture, in the display encoding
//      carried on above 1 (postfx_hdr.hlsli), where the blur, the bloom and the final pass read it like any picture. Light above
//      white is kept, so a lit highlight feeds the glow and reaches the final pass's shoulder.
//   1  the only pass, when no screen effect is on. The result is the finished 8-bit picture: at the picture's place in a texture of
//      the frame target's size (Aux.zw = where) or in a texture of the picture's size (Aux.zw = 0), with the frame target's alpha.
//      What the light pushes over white rolls off with a shoulder that starts at the pixel's own brightness before the light (never
//      below Mode.z), keeps the hue, and reaches white at Mode.w; so the roll-off begins exactly where the light begins to add.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#include "postfx_common.hlsli"
#include "postfx_hdr.hlsli"

COMBINED Texture2D<float4> Field : register(t0, space2);
COMBINED SamplerState FieldSamp : register(s0, space2);
COMBINED Texture2D<float4> Light : register(t1, space2);
COMBINED SamplerState LightSamp : register(s1, space2);

float4 main(PSInput input) : SV_Target0
{
    const float2 p = input.Position.xy - Aux.zw; // this pixel in the picture
    const int2 q = int2(Src.xy) + int2(p);       // and in the frame target and the light buffer
    const float4 field = Field.Load(int3(q, 0));
    const float3 light = Light.Load(int3(q, 0)).rgb;
    const bool unlit = light.r == 1.0f && light.g == 1.0f && light.b == 1.0f;
    const float3 base = SrgbToLinear(field.rgb);
    float3 c = base * max(light, 0.0f);
    if (Mode.x < 0.5f)
        return float4(unlit ? field.rgb : LinearToSrgbUncut(c), 1.0f);

    if (unlit)
        return field;
    const float m = max(c.r, max(c.g, c.b));
    const float t = max(Mode.z, max(base.r, max(base.g, base.b)));
    if (m > t && t < 0.999f)
    {
        // The extended Reinhard curve of postfx_final.frag.hlsl, starting at t.
        const float s = 1.0f - t;
        const float u = (m - t) / s;
        const float uw = (Mode.w - t) / s;
        const float f = t + s * u * (1.0f + u / (uw * uw)) / (1.0f + u);
        c *= min(f, 1.0f) / m;
    }
    return float4(LinearToSrgb(c), field.a);
}
