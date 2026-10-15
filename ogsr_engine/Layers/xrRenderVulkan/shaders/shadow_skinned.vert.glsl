#version 450
// Sun shadow caster for SKINNED dynamics (depth only) — see shadow_skinned_body.glsl.
// pc.mvp is the LIGHT view*proj here. No fragment stage.
#extension GL_GOOGLE_include_directive : require
#include "shadow_skinned_body.glsl"
