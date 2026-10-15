#ifndef LIGHT_ATTEN_GLSL
#define LIGHT_ATTEN_GLSL
// xrRenderVulkan — everything about ONE dynamic point/spot light that needs no
// shadow map and no descriptor: distance falloff, spot cone, and the wrapped N·L
// narrow beams use. ONE copy.
//
// It is split out because the RT world now shades with the same lights the raster
// does (the probe rays in ddgi_trace.comp.glsl), and that shader can reach none of
// what light_shade.glsl depends on — no `L` UBO, no shadow atlases, no cluster
// SSBOs. Left un-split, the probe pass would grow its own falloff and its own cone,
// and the bounce would then disagree with the direct light it is supposed to be a
// bounce OF. Every such copy in this renderer has drifted; this one is not made.
//
// ⚠ Returns the attenuation WITHOUT N·L and hands N·L back separately, because the
// raster caller multiplies shadowing into the attenuation and only then applies the
// cosine. Folding them here would reassociate the product and shift the raster's
// last bits for no reason — the SPIR-V gate would (correctly) call that a change.

// `ld`   — unit direction from the shaded point TO the light,
// `dist` — distance to it (an RT caller needs it to bound the shadow ray),
// `ndl`  — the (possibly wrapped) cosine, NOT clamped.
// Returns 0 when the point is out of range, in which case ld/dist/ndl are junk.
float dynLightAtten(vec4 lpos, vec4 lcol, vec4 ldir, vec3 wp, vec3 N,
                    out vec3 ld, out float dist, out float ndl)
{
    const vec3  dv = lpos.xyz - wp;
    const float r  = lpos.w;
    const float d2 = dot(dv, dv);
    ld = vec3(0.0, 1.0, 0.0); dist = 0.0; ndl = 0.0;
    if (d2 >= r * r) return 0.0;

    const float d = sqrt(max(d2, 1e-6));
    ld   = dv / d;
    dist = d;

    // Distance falloff. NARROW beams (headlights/searchlights): windowed
    // (1-(d/r)^2)^2 — (1-d/r)^2 is basically dead past 60% range, so a 35 m
    // headlight painted nothing beyond ~20 m. Wide lamps keep the classic curve.
    float att;
    if (lcol.w > 0.5 && ldir.w > 0.87) {
        att = 1.0 - (d2 / (r * r));
        att *= att;
    } else {
        att = 1.0 - d / r;
        att *= att;
    }
    if (lcol.w > 0.5) {   // spot cone
        const float ca = dot(-ld, ldir.xyz);
        // NARROW beams (cos(half) > 0.87): full strength anywhere INSIDE the cone
        // plus a spill fade out to 2x the angle (real fixtures spill well past the
        // bright core). The default ramp peaks only ON the axis, so the ground pool
        // a headlight visibly paints sat at the cone edge at ~0.
        if (ldir.w > 0.87) {
            const float co = 2.0 * ldir.w * ldir.w - 1.0;   // cos(2*half)
            att *= clamp((ca - co) / max(ldir.w - co, 1e-3), 0.0, 1.0);
        } else
            att *= clamp((ca - ldir.w) / max(1.0 - ldir.w, 1e-3), 0.0, 1.0);
    }

    ndl = dot(N, ld);
    // NARROW beams: wrap the diffuse — a near-horizontal beam grazes the ground at
    // N·L ~ 0.05 and painted no light pool where it visibly lands. Wide spots
    // (flashlight 70 deg, pole lamps 120 deg) keep plain Lambert.
    if (lcol.w > 0.5 && ldir.w > 0.87) ndl = (ndl + 0.4) * (1.0 / 1.4);
    return att;
}

#endif // LIGHT_ATTEN_GLSL
