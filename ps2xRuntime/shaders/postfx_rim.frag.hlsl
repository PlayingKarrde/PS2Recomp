// Wrestler shape: the outline of the whole wrestler and a rim of light (task lt3-T1, gpu_postfx.cpp; docs/hd-lighting-tier3.md
// in the game's repository; the C++ reference of the arithmetic is ps2x_lighting::wrestlerShapeAt in gpu/gpu_lighting.cpp).
//
// A wrestler is flat pictures, so the lit shader can only tint him by where he stands. This pass gives him a little shape from
// his outline on the screen: near the outline the surface is taken to lean outward, as it would on a rounded body, and the lights
// are worked out again for that leaning surface. It runs once per field, at the end of the scene, and writes a copy of the
// frame's light buffer in which the pixels of the wrestlers carry the new light; the multiply pass (postfx_light.frag.hlsl) then
// reads that copy. Every other pixel is copied as it is.
//
// Inputs (all the size of the frame target except Blur):
//   Light   the light buffer of the lit draws (half-float, linear light)
//   Where   written by the wrestler cards (gs_card.hlsli): rgb = the pixel's place in the arena, a = mask channel (1 to 4) plus
//           half the share of the lighting the pixel takes; 0 where the pixel is not a card
//   Blur    the blurred wrestler mask (postfx_rim_blur.frag.hlsl), the size of the picture, one channel per wrestler
// Parameters (the block of every pass, read in this pass's own way):
//   Src.xy   where the picture starts in the frame target
//   Src.zw   the picture's size
//   Aux.xyz  the camera's position (world units); Aux.w: +1 or -1, whether cross(up, view) points to the right of the screen
//   Tilt.x   shape strength (0 = today's tint by position, 1 = the full effect)
//   Tilt.y   rim strength
//   Tilt.w   how far the surface leans at the very outline (0.92)
// and the lights of the look, in the block of the lit shaders (gs_light.hlsli; the two declarations must stay the same).
//
// The arithmetic, per wrestler pixel (route (d) of the study):
//   b       the blurred mask of the pixel's own wrestler; lean = saturate((1 - b) x 2) x 0.92, towards where b falls
//   N       the surface direction: the camera's view of a flat-on surface, tipped by `lean` in the screen's plane
//   shaped  ambient + sum over the lights of (light x wrap(N.L)) x gain, wrap(x) = saturate((x + 1/2) / (3/2)); the gain makes a
//           flat-on pixel under the look's first light exactly as bright as it is today
//   rim     sum over the lights of (light x max(N2.L, 0)) x lean^2, N2 = the lean's direction lying in the screen's plane
//   result  today's light + shape strength x (shaped - today's light) + rim strength x rim, times the pixel's share
//
// The body is in postfx_rim.hlsli, shared with postfx_rim_n.frag.hlsl (task lt3-T3: the same pass with the generated normal maps).
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 3
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 2

#include "postfx_rim.hlsli"
