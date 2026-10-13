// xrRenderVulkan — r_grass_debug lighting-component isolation views.
//
// NOT a standalone header: this is a BODY include (same contract as
// vsm_grass_page_body.glsl). detail.frag #include's it AFTER everything it
// reads is declared — the grass varyings (vWPos, vHeight, vHemi, vSunOcc),
// the shared push block (detail_gfx_push.glsl), light_ubo's L, the VSM direct
// taps (vsm_sample.glsl with VSM_GRASS_DIRECT) and sunShadow1. Debug-only
// code: when the grass grows a world-state variant mask (tree_variants.glsl
// pattern), everything here goes behind the debug term in one cut.
#ifndef DETAIL_DEBUG_GLSL
#define DETAIL_DEBUG_GLSL

// r_grass_debug 1..11 (L.spot_params.z): paint the GRASS ONLY with one isolated
// lighting component, greyscale — the rest of the scene renders normally, so a
// pattern "seen through the bush" can be attributed to its actual source. This
// tool closed the 2026-07-04 "shadows through the grass" saga (root cause: the
// screen-space VSM mask belongs to the surface BEHIND the blade) — keep it.
//  1 = sun shadow factor the blade APPLIES (VSM direct atlas @ vWPos / cascade tap)
//  2 = VSM mask R  (FULL shadow, resolved at the surface BEHIND the blade)
//  3 = VSM mask B  (STATIC-only channel — the abandoned mask-based grass path)
//  4 = mask parallax: R = how far behind the blade the mask's surface sits, B = blade height
//  5 = GTAO of the ground behind (ambient gate)
//  6 = SSIL bounce of the ground behind (ambient boost)
//  7 = BAKED per-slot sun occlusion (vSunOcc: level "lightmap" on grass)
//  8 = BAKED per-slot hemi occlusion (vHemi: level "lightmap", ambient side)
//  9 = full ambient term (sky*hemi + floor, x AO x SSIL)
// 10 = dynamic lights on grass (incl. their pool SHADOWS)
// 11 = shadow SOURCE split: white=lit, grey=static shadow (trees/buildings),
//      RED=dyn-atlas shadow (grass-on-grass / NPC) — reacts to r_grass_self_bias live
vec3 grassDebugColor(int gdbg, vec3 albedo)
{
    vec2  suv = gl_FragCoord.xy * L.ao_params.xy;
    vec4  m   = texture(uVsmMask, suv);
    float par = smoothstep(0.15, 0.6, m.g - length(vWPos - L.eye_pos.xyz));
    if (gdbg == 1) return vec3((L.shadow_params.w > 0.5) ? vsmSunShadowGrassDirect(vWPos, L.spot_params.w) : sunShadow1(vWPos));
    if (gdbg == 2) return vec3(m.r);
    if (gdbg == 3) return vec3(m.b);
    if (gdbg == 4) return vec3(par, 0.0, clamp(vHeight, 0.0, 1.0));
    if (gdbg == 5) return vec3(gtaoVis());
    if (gdbg == 6) return ssilBoost() - vec3(1.0);
    if (gdbg == 7) return pc.vSunColor.rgb * vSunOcc / max(max(pc.vSunColor.r, max(pc.vSunColor.g, pc.vSunColor.b)), 1e-4);
    if (gdbg == 8) return vec3(vHemi);
    if (gdbg == 9) return (skyAmbientUp() * (vHemi * L.sky_params.y) + L.ambient.rgb)
                          * coloredAO(gtaoVis(), albedo) * ssilBoost();
    if (gdbg == 10) return dynLightsFoliage(vWPos);
    if (gdbg == 11) {
        float dynHit = 0.0;
        float lit = (L.shadow_params.w > 0.5) ? vsmSunShadowGrassDirectDbg(vWPos, L.spot_params.w, dynHit)
                                              : sunShadow1(vWPos);
        return mix(vec3(mix(0.25, 1.0, lit)), vec3(1.0, 0.0, 0.0), dynHit);
    }
    return vec3(1.0, 0.0, 1.0);   // unknown mode
}

#endif // DETAIL_DEBUG_GLSL
