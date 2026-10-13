#version 450
// Thin wrapper — the actual shader lives in world_vert_body.glsl, shared by all
// six world static VS variants (lmap/vlit x plain/_fade/_inst). WORLD_VLIT swaps
// the baked-lighting source to the per-vertex D3DCOLOR + packed NORMAL alpha.
#extension GL_GOOGLE_include_directive : require
#define WORLD_VLIT 1
#include "world_vert_body.glsl"
