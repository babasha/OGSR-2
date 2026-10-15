#version 450
// Thin wrapper — the actual shader lives in world_vert_body.glsl, shared by all
// six world static VS variants (lmap/vlit x plain/_fade/_inst).
#extension GL_GOOGLE_include_directive : require
#include "world_vert_body.glsl"
