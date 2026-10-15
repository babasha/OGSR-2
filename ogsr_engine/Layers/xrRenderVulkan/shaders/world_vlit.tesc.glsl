#version 450
// Thin wrapper — the shared tesc body lives in world_tesc_body.glsl.
// WORLD_VLIT swaps the pass-through interface to the vert-lit attribute set.
#extension GL_GOOGLE_include_directive : require
#define WORLD_VLIT 1
#include "world_tesc_body.glsl"
