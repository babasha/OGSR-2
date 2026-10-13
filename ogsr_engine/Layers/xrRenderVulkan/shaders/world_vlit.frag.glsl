#version 450
// Thin wrapper — vert-lit variant of the shared world static FS. WORLD_VLIT swaps
// the baked-occlusion source (packed NORMAL alpha instead of the lightmap) and
// adds the baked vertex-lighting term; see world_frag_body.glsl.
#extension GL_GOOGLE_include_directive : require
#define WORLD_VLIT 1
#include "world_frag_body.glsl"
