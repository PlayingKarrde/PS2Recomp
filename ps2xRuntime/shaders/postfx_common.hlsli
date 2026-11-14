// What every pass of the screen effects has in common (task fx1, gpu_postfx.cpp): the input of the fragment shaders and the
// parameter block. The same 128 bytes are pushed for every pass; each pass reads the members it needs (the comments say which).
// The C++ side is struct PassParams in gpu_postfx.cpp: change both together.
//
// Register spaces follow SDL_gpu.h, comment on SDL_CreateGPUShader: fragment shaders use space2 for sampled textures and samplers
// and space3 for uniform buffers. SDL's Vulkan backend binds one combined image sampler per sampler slot, so the texture and the
// sampler of a slot carry [[vk::combinedImageSampler]] under __spirv__ (see present.frag.hlsl).

#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#define LOCATION(n) [[vk::location(n)]]
#else
#define COMBINED
#define LOCATION(n)
#endif

// postfx.vert.hlsl writes the texture coordinate first and SV_Position after it (see field_select.frag.hlsl for why). The pass
// reads the pixel it draws from Position.xy: texels of the target, the centre of pixel x is x + 0.5.
struct PSInput
{
    LOCATION(0) float2 UV : TEXCOORD0;
    float4 Position : SV_Position;
};

cbuffer Params : register(b0, space3)
{
    float4 Src;    // x, y: where the picture starts in the texture the pass reads (texels); z, w: the picture's size in texels
    float4 Aux;    // pass specific: tilt: x, y = the blur direction; final: x, y = where the picture starts in the frame target, z, w = where it goes in the output
    float4 Tilt;   // tilt-shift passes: x: focus centre (0..1 down the picture), y: half the focus band (0..1), z: the fall-off width (0..1), w: blur radius at full blur (texels)
                   // final pass (it does not blur): x = 1 when the grading is on, y = 1 when the vignette is on (fx2: an effect that is off is not computed)
    float4 Bloom;  // x: threshold (LINEAR light), y: soft knee (0..1 of the threshold), z: intensity, w: how much of each smaller level is added back
    float4 GradeA; // x: exposure (stops), y: contrast, z: saturation, w: temperature (-1 cool .. 1 warm)
    float4 GradeB; // x: lift, y: gamma, z: gain, w: unused
    float4 Vig;    // x: strength, y: radius (0..1, 1 = the corners), z: softness, w: width over height of the picture as displayed
    float4 Mode;   // pass specific: down: x = 1 is the first step (reads the 8-bit picture and applies the bloom threshold);
                   // final: y = 1 adds bloom, z = where the highlight shoulder starts (linear, >= 1 means none), w = the white point (linear)
};

// The scene is an 8-bit picture in the display encoding (sRGB); every effect that adds or mixes light does it in linear light, where
// that is physically right and where a sum that goes over 1.0 is still a number that can be rolled off instead of cut (fx2).
float3 SrgbToLinear(float3 c)
{
    const float3 low = c / 12.92f;
    const float3 high = pow((c + 0.055f) / 1.055f, 2.4f);
    return lerp(high, low, step(c, 0.04045f));
}

float3 LinearToSrgb(float3 c)
{
    c = saturate(c);
    const float3 low = c * 12.92f;
    const float3 high = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(c, 0.0031308f));
}

// 0 in the focus band, rising to 1 over the fall-off width (v: 0..1 down the picture).
float TiltAmount(float v)
{
    const float distance = abs(v - Tilt.x) - Tilt.y;
    return smoothstep(0.0f, max(Tilt.z, 1e-4f), distance);
}
