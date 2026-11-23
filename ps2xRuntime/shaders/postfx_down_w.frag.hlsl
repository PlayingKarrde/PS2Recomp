// Bloom, going down, first step only, with the test switches of task fx3 (gpu_postfx.cpp; PS2X_FX_BLOOM_LUMA, PS2X_FX_BLOOM_WRESTLER).
//
// It is postfx_down.frag.hlsl (see there: the 13-tap filter, the threshold with its soft knee, linear light) with two changes in the first
// step, the one that reads the picture and applies the threshold. The later steps use postfx_down.frag.hlsl as before; the chain uses this
// shader only for the first step and only when a switch is on, so with both switches at their defaults no picture changes.
//   Aux.x   1: the threshold measures the pixel's brightness as Rec.709 luminance (0.2126 r + 0.7152 g + 0.0722 b) instead of its largest
//           channel. A saturated warm skin tone (a high red, a low green and blue) is then not "bright" for the threshold, a white or a
//           grey is as before. The soft knee and the colour of what passes are as in the max-channel form.
//   Aux.y   below 1: what passes the threshold is multiplied by this on the wrestlers' pixels (the glow of a wrestler is Aux.y times as
//           strong); 1 leaves the pixels alone.
//   Aux.zw  where the picture starts in the wrestler mask (the mask is the frame target's size)
//   Mask    the wrestler mask (8 bits, any channel above 0.5 is a wrestler pixel); read only when Aux.y is below 1
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 2
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1

#define DOWN_WRESTLER
#include "postfx_down.hlsli"
