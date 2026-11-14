// The match lights at one pixel (task lt2-L1; gpu/gpu_lighting.h has the same arithmetic in C++, lightAt and lookAt, and the unit
// tests hold the two together). Included by the lit fragment shaders after the body of the plain shader.
//
// The light is what multiplies the game's own colour: ambient plus every light of the look that is on (up to 8, spot or point).
// It is linear light and may be above 1.

#define MAX_LIGHTS 8

cbuffer Light : register(b1, space3)
{
    float4 LightAmbient;                // rgb; w: how many lights follow
    float4 LightRule;                   // x: how the surface's direction counts for this draw: 0 Lambert max(N.L, 0), 1 half (N.L / 2 + 1/2)
                                        //    squared, 2 by position only; y: 1 = draw the markers of the lights ("show lights")
    float4 LightPosition[MAX_LIGHTS];   // xyz: where the light is (world units), w: its range (no light beyond it)
    float4 LightAxis[MAX_LIGHTS];       // xyz: the unit direction a spotlight points in, w: its intensity
    float4 LightColour[MAX_LIGHTS];     // rgb, w: 1 a spotlight, 0 a point light
    float4 LightCone[MAX_LIGHTS];       // x: cos(outer half angle), y: cos(inner half angle), w: the light's number in the look
};

// "Show lights" (the debug panel): marks on the lit surfaces that show where each light is and what it covers. A mark is written
// into the light buffer as a NEGATIVE colour, which no real light can be; the multiply pass draws such a pixel in that colour
// (postfx_light.frag.hlsl). The marks of light k, in its own colour:
//   - a filled dot on whatever is straight under (or over) the light;
//   - a spotlight: the outline of its outer cone and, thinner, of its inner cone, and a small ring where it aims;
//   - a point light: the outline of its range.
float3 MarkerColour(float number)
{
    const float3 palette[8] = {float3(1.0f, 0.25f, 0.2f), float3(0.2f, 0.9f, 0.3f), float3(0.3f, 0.5f, 1.0f), float3(1.0f, 0.85f, 0.2f),
                               float3(1.0f, 0.3f, 0.9f), float3(0.2f, 0.9f, 0.9f), float3(1.0f, 0.55f, 0.15f), float3(0.75f, 0.75f, 0.75f)};
    return palette[(int)clamp(number, 0.0f, 7.0f)];
}

bool LightMarker(float3 world, out float3 colour)
{
    colour = float3(0.0f, 0.0f, 0.0f);
    const int count = (int)LightAmbient.w;
    for (int k = 0; k < MAX_LIGHTS; ++k)
    {
        if (k >= count)
            break;
        const float3 toLight = LightPosition[k].xyz - world;
        const float distance = length(toLight);
        bool mark = length(toLight.xz) < 9.0f; // straight under the light
        if (LightColour[k].w > 0.5f)
        {
            if (distance > 1.0f)
            {
                const float angle = degrees(acos(clamp(-dot(LightAxis[k].xyz, toLight / distance), -1.0f, 1.0f)));
                const float outer = degrees(acos(clamp(LightCone[k].x, -1.0f, 1.0f)));
                const float inner = degrees(acos(clamp(LightCone[k].y, -1.0f, 1.0f)));
                mark = mark || abs(angle - outer) < 0.45f || abs(angle - inner) < 0.2f;
                // where it aims: the surface points near the axis, as a small ring
                const float off = distance * sin(radians(min(angle, 89.0f)));
                mark = mark || (angle < 60.0f && off > 10.0f && off < 14.0f);
            }
        }
        else
        {
            mark = mark || abs(distance - LightPosition[k].w) < 4.0f;
        }
        if (mark)
        {
            colour = MarkerColour(LightCone[k].w);
            return true;
        }
    }
    return false;
}

float3 LightAtPixel(float3 world, float3 normal)
{
    float3 light = LightAmbient.rgb;
    const int count = (int)LightAmbient.w;
    const float3 n = normalize(normal);
    for (int k = 0; k < MAX_LIGHTS; ++k)
    {
        if (k >= count)
            break;
        const float3 toLight = LightPosition[k].xyz - world;
        const float distance = length(toLight);
        if (distance >= LightPosition[k].w)
            continue;
        const float ratio = distance / LightPosition[k].w;
        const float fall = (1.0f - ratio * ratio) * (1.0f - ratio * ratio);
        const float3 l = distance > 0.0f ? toLight / distance : float3(0.0f, 1.0f, 0.0f);
        float cone = 1.0f;
        if (LightColour[k].w > 0.5f)
            cone = smoothstep(LightCone[k].x, LightCone[k].y, -dot(LightAxis[k].xyz, l));
        float face = 1.0f;
        if (LightRule.x < 1.5f)
        {
            const float nl = dot(n, l);
            face = LightRule.x < 0.5f ? max(nl, 0.0f) : (nl * 0.5f + 0.5f) * (nl * 0.5f + 0.5f);
        }
        light += LightColour[k].rgb * (LightAxis[k].w * fall * cone * face);
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
    const float3 world = input.WorldQ * invQ;
    // The draw's mode (LightRule.x): 3 is a fixed light of 1 + glow (a material that is unlit or glows, or an effect); otherwise the
    // lights, of which the draw takes the share LightRule.z (1 = all; a material's strength).
    float3 light;
    if (LightRule.x > 2.5f)
        light = float3(1.0f, 1.0f, 1.0f) * (1.0f + LightRule.w);
    else
        light = lerp(float3(1.0f, 1.0f, 1.0f), LightAtPixel(world, input.Normal), LightRule.z);
    float3 marker;
    if (LightRule.y > 0.5f && LightMarker(world, marker))
        light = -marker;
    output.Light = float4(light, output.Colour.a);
    return output;
}
