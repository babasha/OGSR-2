// xrRenderVulkan — the screen-space AO / indirect-light taps.
//
// These are NOT pure (ao_common.glsl is): they read uAO / uIL / L.ao_params, so
// CONTRACT: #include AFTER light_ubo.glsl, which declares all three and carries the
// ENV_SET indirection (set 1 for the world, set 2 for tree/skinned). That macro is
// why these can be shared at all.
//
// They live here rather than in env_common.glsl because env_common needs
// shadow_common (rainVis -> cascTap), which the foliage passes do not have — the
// same reason sky_ambient.glsl and ao_common.glsl were split out. Copying them per
// pass is how detail/tree/env each ended up with their own.
#ifndef AO_SAMPLED_GLSL
#define AO_SAMPLED_GLSL

// GTAO visibility (R4 combine_1.ps: occludes hemi+ambient only, never sun/dyn).
// Strength is an EXPONENT: 0 = off (->1.0), 1 = raw, 2-3 deepens corners.
//
// ⚠ The exponent is passed IN, not scaled inside. gtaoVis() must hand
// L.ao_params.z straight through: writing it as gtaoVisK(1.0) puts a literal
// `* 1.0` into world_lmap/world_vlit/world_terrain — the per-pixel shaders that
// cover the frame — and the SPIR-V diff shows it as a real extra OpFMul. The
// callers that DO scale (foliage: half strength, because alpha-tested leaf depth
// is noisy at half-res and the full exponent reads as speckle) pay for it alone.
float gtaoVisExp(float e)
{
    float ao = textureLod(uAO, gl_FragCoord.xy * L.ao_params.xy, 0.0).r;
    return pow(clamp(ao, 0.0, 1.0), e);
}
float gtaoVis()               { return gtaoVisExp(L.ao_params.z); }
float gtaoVisK(float expScale) { return gtaoVisExp(L.ao_params.z * expScale); }

// SSIL: multiply ambient by (1 + il/(1+il)) — a soft, energy-limited boost that is
// exactly 1.0 (a no-op) when there is no bounce or r_ssil is off, because the pass
// then binds the BLACK fallback. uIL is binding 21 (light_ubo.glsl).
vec3 ssilBoost()
{
    vec3 il = textureLod(uIL, gl_FragCoord.xy * L.ao_params.xy, 0.0).rgb;
    // NaN FIREWALL on a FEEDBACK LOOP: the SSIL history is a snapshot blitted
    // off the scene image, and this boost multiplies back into that same scene
    // — one non-finite pixel re-seeds itself every frame and never heals (on
    // screen: persistent flickering black patches, worst right after a level
    // load seeds garbage). clamp bounds Inf and the negative half (il = -1
    // would divide by zero right here); the select is what actually kills NaN
    // — min/max against NaN is undefined in SPIR-V, clamp alone can pass it.
    il = mix(clamp(il, vec3(0.0), vec3(64.0)), vec3(0.0), isnan(il));
    return vec3(1.0) + il / (1.0 + il);
}

#endif // AO_SAMPLED_GLSL
