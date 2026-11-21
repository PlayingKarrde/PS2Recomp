// The third output of a wrestler card (task lt3-T1): included by gs_card.frag.hlsl and gs_rep_card.frag.hlsl after the body of the
// plain shader and gs_light.hlsli. See gs_card.frag.hlsl for what the output means.

//
// With CARD_NORMALS defined (task lt3-T3: gs_card_n.frag.hlsl, gs_rep_card_n.frag.hlsl) there is a fourth output, read from the
// tile's generated normal map (made by ps2x_lighting::generateTileNormals; one more texture, in the slot CARD_NORMAL_T /
// CARD_NORMAL_S, of the size of the tile's own texture). The map says, in the texture's own axes, which way the surface leans at
// each texel. The part may be drawn rotated, mirrored or upside down, so the shader finds where the texture's two axes point in
// the arena at this pixel (from how the texel coordinates and the world position change from one screen pixel to the next) and
// turns the map's lean with them. The picture shows the wrestler as the camera sees him, so "right on the card" then counts as
// right on the screen and "up on the card" as up on the screen. The output is that lean with x to the right of the screen and y
// down it, for the shape pass (postfx_rim.hlsli):
//   rg  the rounded-edge ("bulge") part of the map;   ba  the painted light and dark part.   Each as 128/255 + value x 127/255.
// Without CARD_NORMALS this file compiles to exactly what T1 had.

struct CardOutput
{
    float4 Colour : SV_Target0; // the game's colour, exactly as the plain shader writes it
    float4 Light : SV_Target1;  // the light at this pixel by its position, exactly as the lit shader writes it
    float4 Where : SV_Target2;  // rgb: the pixel's place in the arena; a: mask channel + share / 2 (0: not a card)
#ifdef CARD_NORMALS
    float4 Lean : SV_Target3;   // the generated map's lean at this pixel, turned to the screen (see above)
#endif
};

#ifdef CARD_NORMALS
COMBINED Texture2D<float4> NormalTex : register(CARD_NORMAL_T, space2);
COMBINED SamplerState NormalSamp : register(CARD_NORMAL_S, space2);

// The map's lean at this pixel, turned to the screen. Called before anything can discard the pixel: it takes screen derivatives.
float4 CardLean(PSInput input)
{
    const float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);
    const float2 uv = (Flags.x & 2u) != 0u ? input.UVQ.xy : input.UVQ.xy * invQ * TexInfo.xy; // texels
    const float3 world = input.WorldQ * invQ;
    const float2 duvdx = ddx(uv), duvdy = ddy(uv);
    const float3 dwdx = ddx(world), dwdy = ddy(world);
    // The sampler is nearest (the light steps with the pixel art) or linear (smooth), as the look asks: the renderer binds one or the other.
    const float4 map = NormalTex.SampleLevel(NormalSamp, uv * TexInfo.zw, 0.0f) * 255.0f;
    const float2 bulge = (map.xy - 128.0f) / 127.0f;
    const float2 paint = (map.zw - 128.0f) / 127.0f;
    // Where one texel to the right (T) and one texel down (B) in the texture lead in the arena.
    const float det = duvdx.x * duvdy.y - duvdy.x * duvdx.y;
    float4 lean = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (abs(det) > 1.0e-12f)
    {
        float3 T = (dwdx * duvdy.y - dwdy * duvdx.y) / det;
        float3 B = (dwdy * duvdx.x - dwdx * duvdy.x) / det;
        const float tl = length(T), bl = length(B);
        float3 right = float3(dwdx.x, 0.0f, dwdx.z); // to the right of the screen, along the (upright) card
        const float rl = length(right);
        if (tl > 1.0e-9f && bl > 1.0e-9f && rl > 1.0e-9f)
        {
            T /= tl;
            B /= bl;
            right /= rl;
            const float2 tr = float2(dot(T, right), dot(B, right)); // how far each texture axis goes to the right ...
            const float2 tu = float2(T.y, B.y);                     // ... and up
            lean = float4(dot(bulge, tr), -dot(bulge, tu), dot(paint, tr), -dot(paint, tu));
        }
    }
    return saturate((lean * 127.0f + 128.0f) / 255.0f);
}
#endif

CardOutput CardPixel(PSInput input)
{
#ifdef CARD_NORMALS
    const float4 lean = CardLean(input);
#endif
    const LitOutput lit = LitPixel(input); // (a pixel that fails the alpha test is discarded in there, for all the targets)
    CardOutput output;
    output.Colour = lit.Colour;
    output.Light = lit.Light;
    const float invQ = 1.0f / (abs(input.UVQ.z) > 1.0e-8f ? input.UVQ.z : 1.0f);
    const float3 world = input.WorldQ * invQ;
    // A marker of "show lights" (a negative light) is left as it is: the pixel is not shaped.
    const bool marker = lit.Light.r < 0.0f || lit.Light.g < 0.0f || lit.Light.b < 0.0f;
    const float share = min(LightRule.z * lit.Colour.a, 1.9f) * 0.5f;
    output.Where = float4(world, marker ? 0.0f : LightRule.w + share);
#ifdef CARD_NORMALS
    output.Lean = lean;
#endif
    return output;
}
