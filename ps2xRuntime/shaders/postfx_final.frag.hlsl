// The last pass of the screen effects (task fx1, fx2, gpu_postfx.cpp): bloom added, colour grading, a soft shoulder for the highlights,
// the vignette.
//
// Inputs: Pic is the picture the effects work on (the tilt-shifted one, or the scene as it was; 8 bits, display encoding), Glow the
// bloom (half size, half-float, LINEAR light, read with linear filtering), Field the frame target (only its alpha is used: the alpha
// of a GS frame is a blend coefficient, not a colour, and the game's own value goes on to the draws that follow).
//
// The pass draws the finished picture either into a texture of its own that is the size of the picture (Aux.zw = 0), or into a
// texture the size of the frame target at the picture's own place in it (Aux.zw = where). The second is what the backend does when
// the game still has its 2D overlay to draw: it draws that overlay a second time into this texture, on top of the processed scene,
// with the game's own blend, so the overlay is never mixed with the effects and needs no mask (gs_gpu_backend.cpp, fxBoundary).
//
// Why linear light, and the order (fx2). fx1 added the glow to the 8-bit display values and cut everything over 1.0: a pixel that was
// already bright went over 1.0 in the red and green channels, was cut there, and came out as a flat yellow or white (skin, the
// mat's logo). Now:
//   1. Pic is made linear (SrgbToLinear) and the glow is added in linear light, which is where light adds up.
//   2. Grading that scales or mixes light: exposure (stops), temperature (red up, blue down), gain, saturation. Any of them can take a
//      pixel over 1.0; that is no longer a problem.
//   3. The shoulder: the pixel's largest channel m is mapped to f(m), where f is the identity below Mode.z and rolls off smoothly
//      above it (an extended Reinhard curve that reaches 1.0 at Mode.w, a white far over 1). All three channels are scaled by the
//      same factor f(m) / m, so the colour keeps its hue (a bright orange stays orange, it does not turn yellow, and then white).
//   4. LinearToSrgb, back to what the display wants.
//   5. Lift, gamma and contrast on the encoded values. The contrast curve is an S that keeps 0 and 1 where they are, so it can never cut.
//   6. The vignette multiplies the encoded values.
// The grading (steps 2 and 5) and the vignette (step 6) can each be switched off (Tilt.x and Tilt.y here: this pass does not use the
// tilt-shift's numbers): the shader then does not compute them at all. The shoulder stays whenever Mode.z says so (the glow or the
// grading adds light that needs it).
// All numbers are neutral at glow 0, exposure 0, contrast 1, saturation 1, temperature 0, lift 0, gamma 1, gain 1, Mode.z >= 1 and
// vignette 0, and then the output is the input, byte for byte (the 8 bits go to linear and back in 32-bit floats).
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

// The shoulder as a factor on the pixel: 1 while the largest channel m is at or below the start (Mode.z), then f(m) / m.
float ShoulderScale(float m)
{
    const float t = Mode.z;
    if (m <= t)
        return 1.0f;
    const float s = 1.0f - t;
    const float u = (m - t) / s;
    const float uw = (Mode.w - t) / s;
    const float f = t + s * u * (1.0f + u / (uw * uw)) / (1.0f + u);
    return min(f, 1.0f) / m;
}

float3 Grade(float3 c, float2 uv)
{
    const bool grading = Tilt.x > 0.5f;  // the switches of the grading and the vignette (gpu_postfx.cpp): an effect that is off is not computed
    const bool vignette = Tilt.y > 0.5f;
    if (grading)
    {
        c *= exp2(GradeA.x);
        c.r *= pow(1.0f + 0.12f * GradeA.w, 2.2f);
        c.b *= pow(1.0f - 0.12f * GradeA.w, 2.2f);
        c *= pow(GradeB.z, 2.2f);
        const float luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
        c = max(lerp(float3(luma, luma, luma), c, GradeA.z), 0.0f);
    }
    if (Mode.z < 0.999f)
        c *= ShoulderScale(max(c.r, max(c.g, c.b)));
    c = LinearToSrgb(c); // saturates: with no shoulder this is the cut of fx1, now at the end of the linear part
    if (grading)
    {
        if (GradeB.x > 0.0f)
            c = c + GradeB.x * (1.0f - c);
        else if (GradeB.x < 0.0f)
            c = c * (1.0f + GradeB.x);
        if (abs(GradeB.y - 1.0f) > 1e-5f)
            c = pow(c, 1.0f / max(GradeB.y, 0.05f));
        if (abs(GradeA.y - 1.0f) > 1e-5f)
        {
            const float3 a = pow(c, GradeA.y);
            const float3 b = pow(1.0f - c, GradeA.y);
            c = a / max(a + b, 1e-9f);
        }
    }
    if (vignette)
    {
        const float2 shape = float2(Vig.w, 1.0f);
        const float d = length((uv - 0.5f) * shape) / length(shape * 0.5f);
        c *= 1.0f - Vig.x * smoothstep(Vig.y, Vig.y + max(Vig.z, 1e-3f), d);
    }
    return saturate(c);
}

float4 main(PSInput input) : SV_Target0
{
    const float2 p = input.Position.xy - Aux.zw;  // this pixel in the picture
    const int2 q = int2(Aux.xy) + int2(p);        // and in the frame target
    const float4 field = Field.Load(int3(q, 0));
    float3 c = SrgbToLinear(Pic.Load(int3(int2(Src.xy) + int2(p), 0)).rgb);
    if (Mode.y > 0.5f)
        c += Glow.SampleLevel(GlowSamp, p / Src.zw, 0.0f).rgb * Bloom.z;
    return float4(Grade(c, p / Src.zw), field.a);
}
