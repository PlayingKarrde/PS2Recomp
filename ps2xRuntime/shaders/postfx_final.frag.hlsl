// The last pass of the screen effects (task fx1, gpu_postfx.cpp): colour grading, bloom added, vignette.
//
// Inputs: Pic is the picture the effects work on (the tilt-shifted one, or the scene as it was), Glow the bloom (half size, read
// with linear filtering), Field the frame target (only its alpha is used: the alpha of a GS frame is a blend coefficient, not a
// colour, and the game's own value goes on to the draws that follow).
//
// The pass draws the finished picture either into a texture of its own that is the size of the picture (Aux.zw = 0), or into a
// texture the size of the frame target at the picture's own place in it (Aux.zw = where). The second is what the backend does when
// the game still has its 2D overlay to draw: it draws that overlay a second time into this texture, on top of the processed scene,
// with the game's own blend, so the overlay is never mixed with the effects and needs no mask (gs_gpu_backend.cpp, fxBoundary).
//
// Order: exposure, temperature, gain and lift, gamma, contrast, saturation, then the vignette. All numbers are neutral at
// exposure 0, contrast 1, saturation 1, temperature 0, lift 0, gamma 1, gain 1, vignette 0.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 3
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#include "postfx_common.hlsli"

COMBINED Texture2D<float4> Pic : register(t0, space2);
COMBINED SamplerState PicSamp : register(s0, space2);
COMBINED Texture2D<float4> Glow : register(t1, space2);
COMBINED SamplerState GlowSamp : register(s1, space2);
COMBINED Texture2D<float4> Field : register(t2, space2);
COMBINED SamplerState FieldSamp : register(s2, space2);

float3 Grade(float3 c, float2 uv)
{
    c *= exp2(GradeA.x);
    c.r *= 1.0f + 0.12f * GradeA.w;
    c.b *= 1.0f - 0.12f * GradeA.w;
    c = c * GradeB.z + GradeB.x;
    c = pow(max(c, 0.0f), 1.0f / max(GradeB.y, 0.05f));
    c = (c - 0.5f) * GradeA.y + 0.5f;
    const float luma = dot(c, float3(0.299f, 0.587f, 0.114f));
    c = lerp(float3(luma, luma, luma), c, GradeA.z);
    const float2 shape = float2(Vig.w, 1.0f);
    const float d = length((uv - 0.5f) * shape) / length(shape * 0.5f);
    c *= 1.0f - Vig.x * smoothstep(Vig.y, Vig.y + max(Vig.z, 1e-3f), d);
    return saturate(c);
}

float4 main(PSInput input) : SV_Target0
{
    const float2 p = input.Position.xy - Aux.zw;  // this pixel in the picture
    const int2 q = int2(Aux.xy) + int2(p);        // and in the frame target
    const float4 field = Field.Load(int3(q, 0));
    float3 c = Pic.Load(int3(int2(Src.xy) + int2(p), 0)).rgb;
    if (Mode.y > 0.5f)
        c += Glow.SampleLevel(GlowSamp, p / Src.zw, 0.0f).rgb * Bloom.z;
    return float4(Grade(c, p / Src.zw), field.a);
}
