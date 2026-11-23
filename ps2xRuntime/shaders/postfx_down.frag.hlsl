// Bloom, going down (task fx1, gpu_postfx.cpp): halves the picture with the 13-tap filter of Jimenez ("Next Generation Post
// Processing in Call of Duty: Advanced Warfare", 2014), whose taps overlap so a bright pixel does not flicker when it moves.
// With Mode.x = 1 (the first step) every tap is first run through the bloom threshold with a soft knee: only what is brighter
// than the threshold goes on to glow.
//
// It draws a texture half the size of the one it reads. Src.xy is where the picture starts in Tex (only the first step reads from
// inside a bigger texture); taps clamp to the picture.
//
// The first step reads the 8-bit picture in the display encoding and makes LINEAR light of it (fx2): a tap is the mean of the linear
// values of the 2 x 2 texels around it (not the linear value of their mean, which is darker at an edge between bright and dark), and the
// threshold (Bloom.x) is a linear value. The later steps read the half-float levels this pass wrote before.
//
// SDL_GPU resource counts, read by build_shaders.py (SDL_CreateGPUShader needs them):
// @samplers 1
// @storage_textures 0
// @storage_buffers 0
// @uniform_buffers 1


// The body is in postfx_down.hlsli, shared with postfx_down_w.frag.hlsl (task fx3).

#include "postfx_down.hlsli"
