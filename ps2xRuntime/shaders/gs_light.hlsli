// The match lights at one pixel (task lt2-L1; gpu/gpu_lighting.h has the same arithmetic in C++, lightAt and lookAt, and the unit
// tests hold the two together). Included by the lit fragment shaders after the body of the plain shader.
//
// The light is what multiplies the game's own colour: ambient plus the spotlight. It is linear light and may be above 1.

cbuffer Light : register(b1, space3)
{
    float4 LightAmbient; // rgb
    float4 SpotPosition; // xyz: where the spotlight is (world units), w: its radius (no light beyond it)
    float4 SpotAxis;     // xyz: the unit direction it points in, w: its strength
    float4 SpotColour;   // rgb
    float4 SpotCone;     // x: cos(outer half angle), y: cos(inner half angle), z: how the surface's direction counts for this draw:
                         //    0 Lambert max(N.L, 0), 1 half (N.L / 2 + 1/2) squared, 2 by position only
};

float3 LightAtPixel(float3 world, float3 normal)
{
    float3 light = LightAmbient.rgb;
    const float3 toLight = SpotPosition.xyz - world;
    const float distance = length(toLight);
    if (SpotPosition.w > 0.0f && distance < SpotPosition.w && SpotAxis.w > 0.0f)
    {
        const float ratio = distance / SpotPosition.w;
        const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
        const float3 l = distance > 0.0f ? toLight / distance : float3(0.0f, 1.0f, 0.0f);
        const float cone = smoothstep(SpotCone.x, SpotCone.y, -dot(SpotAxis.xyz, l));
        float face = 1.0f;
        if (SpotCone.z < 1.5f)
        {
            const float nl = dot(normalize(normal), l);
            face = SpotCone.z < 0.5f ? max(nl, 0.0f) : (nl * 0.5f + 0.5f) * (nl * 0.5f + 0.5f);
        }
        light += SpotColour.rgb * (SpotAxis.w * fall * cone * face);
    }
    return light;
}

struct LitOutput
{
    float4 Colour : SV_Target0; // the game's colour, exactly as the plain shader writes it
    float4 Light : SV_Target1;  // the light at this pixel; its alpha is the same blend coefficient, so it blends as the colour does
};

LitOutput LitPixel(PSInput input)
{
    LitOutput output;
    output.Colour = GsColour(input); // (a pixel that fails the alpha test is discarded in there, for both targets)
    const float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);
    output.Light = float4(LightAtPixel(input.WorldQ * invQ, input.Normal), output.Colour.a);
    return output;
}
