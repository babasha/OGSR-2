#version 450
// Thin wrapper — the actual shader lives in world_frag_body.glsl, shared by all
// four world static FS variants (lmap/vlit x plain/_fade). WORLD_VLIT undefined
// here = the lightmap path.
#extension GL_GOOGLE_include_directive : require
#include "world_frag_body.glsl"
