// Fragment shader of the GS GPU renderer for textures that a texture pack replaces (task t1b; GSGpuBackend, src/lib/gs/gs_gpu_backend.cpp).
//
// It is gs.frag.hlsl with a different way to fetch the texture colour; the colour arithmetic after the fetch (TFX, alpha test,
// the blend coefficient) is the same code, and the two files must stay in step. The draw is the same draw: the primitive, the wrap
// modes, REGION_CLAMP, the 1/16 rounding of coordinates and STQ all work in the ORIGINAL texel units of the declared texture, exactly
// as in gs.frag.hlsl. Only the final fetch changes:
//   - Sampler 0 is the original texture (decoded from GS memory as before). Sampler 1 is the replacement picture: S times the size of
//     the REPLACED RECTANGLE of the texture (RepInfo), either RGBA8 in the GS's alpha scale (0x80 = opaque) or, for an index image,
//     R8 palette indices. An index picture is looked up in the texture's live palette, which comes in the uniform block (Pal).
//   - A tap is a texel of the picture. Its position in original texels is split into the original texel and the sub-texel inside it
//     (S x S per original texel). The wrap mode is applied to the original texel (so REPEAT, CLAMP and REGION_CLAMP behave as the game
//     wants, and a REGION_CLAMP part never reads its neighbour's pixels), and the sub-texel is kept, except that a clamped coordinate
//     takes the edge sub-texel. A tap whose original texel lies outside the replaced rectangle reads the original texture instead.
//   - The colour is filtered bilinearly AFTER the palette lookup (an index image is never filtered as numbers). The colours are
//     weighted by their alpha, so a transparent palette entry (colour black) does not darken the edge of a sprite.
//   - When the picture is minified (several of its texels fall in one target pixel: a high scale on a low internal resolution) the
//     bilinear tap is repeated on a grid across the pixel's footprint (up to 4 x 4), so the result is a smooth average rather than an
//     aliased single tap. The footprint comes from the derivatives of the texel coordinate.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1
//
// Register spaces follow SDL_gpu.h: fragment textures and samplers in space2, uniforms in space3. SDL's Vulkan backend binds one
// COMBINED_IMAGE_SAMPLER per sampler slot, so both declarations of a slot carry [[vk::combinedImageSampler]] when building SPIR-V.

#include "gs_rep_frag_body.hlsli"

float4 main(PSInput input) : SV_Target0
{
    return GsColour(input);
}
