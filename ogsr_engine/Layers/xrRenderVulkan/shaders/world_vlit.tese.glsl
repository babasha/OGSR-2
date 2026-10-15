#version 450
// Thin wrapper — the shared tese body lives in world_tese_body.glsl.
// WORLD_VLIT swaps the pass-through interface to the vert-lit attribute set.
#extension GL_GOOGLE_include_directive : require
#define WORLD_VLIT 1
#include "world_tese_body.glsl"
