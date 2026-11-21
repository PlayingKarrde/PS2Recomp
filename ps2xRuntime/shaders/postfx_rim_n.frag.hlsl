// Wrestler shape with generated normal maps (task lt3-T3, gpu_postfx.cpp; docs/hd-lighting-tier3.md in the game's repository; the
// C++ reference of the arithmetic is ps2x_lighting::wrestlerTileLean and wrestlerShapeAt in gpu/gpu_lighting.cpp).
//
// It is postfx_rim.frag.hlsl (see there: the outline of the whole wrestler, the lights worked out again, the rim) with one more
// input and one more step:
//   Lean    written by the wrestler cards (gs_card.hlsli, CARD_NORMALS): what the generated normal map of the pixel's tile says,
//           turned to the screen. rg: the rounded edge of the tile ("bulge"); ba: the painted light and dark.
//   Bloom.x how much of the map's lean is added (the look's wrestler_normals)
//   Bloom.y how much the painted light and dark counts (wrestler_paint)
//   Bloom.z the crease fade (wrestler_crease)
//   Bloom.w the outline's width in wrestler texels (wrestler_outline: the sigma of the mask's blur in those texels)
//   Tilt.z  the size of one wrestler texel on the screen, in texels of the frame target (task lt3-T2, below)
// The step, per wrestler pixel:
//   A tile's bulge is strongest at the tile's own cut-out. Where that cut-out is the wrestler's outline, that is right. Where it
//   lies inside the wrestler (a joint, a limb's round end tucked over the body) it would draw a crease. The blurred mask says how
//   far the pixel is from the wrestler's outline (dOut), the bulge's own strength how far from its tile's edge (dTile). When dOut
//   is larger than dTile by 1 to 3 texels the bulge is faded out. The painted part is not faded.
//   (task lt3-T2) A card that carries the game's tag also says which part class it is (in the alpha of Where, above the mask
//   channel). For such a pixel the pass looks just past the part's own edge: if another tagged part of the same wrestler lies there
//   and the two are not joined in the body (a forearm over the chest, not a thigh over the hips), the bulge is kept. Cards
//   without a tag are treated exactly as before.
//   The surface is then tipped by the outline's lean plus the map's, and lit as in postfx_rim.frag.hlsl. The rim is the outline's.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 4
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#define RIM_NORMALS
#include "postfx_rim.hlsli"
