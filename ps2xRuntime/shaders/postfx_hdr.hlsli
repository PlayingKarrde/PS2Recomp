// Pictures brighter than white (match lights, task lt2-L1). The passes of the screen effects read pictures in the display
// encoding and make them linear themselves (SrgbToLinear). A lit scene can be brighter than 1.0, which 8 bits cannot hold, so
// it travels in a half-float texture in the same encoding carried on above 1: the curve of LinearToSrgb without the cut.
// SrgbToLinear undoes it exactly for any value, so a pass that reads such a picture needs no change.
float3 LinearToSrgbUncut(float3 c)
{
    c = max(c, 0.0f);
    const float3 low = c * 12.92f;
    const float3 high = 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, step(c, 0.0031308f));
}
