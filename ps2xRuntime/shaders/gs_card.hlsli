// The third output of a wrestler card (task lt3-T1): included by gs_card.frag.hlsl and gs_rep_card.frag.hlsl after the body of the
// plain shader and gs_light.hlsli. See gs_card.frag.hlsl for what the output means.

struct CardOutput
{
    float4 Colour : SV_Target0; // the game's colour, exactly as the plain shader writes it
    float4 Light : SV_Target1;  // the light at this pixel by its position, exactly as the lit shader writes it
    float4 Where : SV_Target2;  // rgb: the pixel's place in the arena; a: mask channel + share / 2 (0: not a card)
};

CardOutput CardPixel(PSInput input)
{
    const LitOutput lit = LitPixel(input); // (a pixel that fails the alpha test is discarded in there, for all three targets)
    CardOutput output;
    output.Colour = lit.Colour;
    output.Light = lit.Light;
    const float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);
    const float3 world = input.WorldQ * invQ;
    // A marker of "show lights" (a negative light) is left as it is: the pixel is not shaped.
    const bool marker = lit.Light.r < 0.0f || lit.Light.g < 0.0f || lit.Light.b < 0.0f;
    const float share = min(LightRule.z * lit.Colour.a, 1.9f) * 0.5f;
    output.Where = float4(world, marker ? 0.0f : LightRule.w + share);
    return output;
}
