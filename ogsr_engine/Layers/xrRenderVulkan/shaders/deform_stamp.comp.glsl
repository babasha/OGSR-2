#version 450
// xrRenderVulkan - SNOW/MUD deform texture: stamp / decay (compute, TOROIDAL in-place).
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
// SPDX-License-Identifier: MIT
//
// A PERSISTENT top-down compression field, WORLD-ANCHORED (toroidal). SIGNED R16F:
// + = pressed DOWN (foot dent), − = pushed UP (the displaced-snow BERM around it).
// Each texel maps to a fixed WORLD XZ (mod kWorld) — so a print stays put in the world
// and NO per-frame reprojection is needed (that was the camera-centred design's cost).
// As the camera moves, only the thin leading strip of texels whose world identity just
// flipped (scrolled in from kWorld away) is CLEARED; everything else just decays toward
// 0. The whole thing runs IN PLACE on one image (imageLoad -> imageStore), so the old
// scratch + 8 MB copy-back is gone too. Stamps come from a BUFFER (no push-constant
// count cap, so many NPCs all print) and are GROUND-GATED against the terrain height
// (rain map) so a lifted foot / airborne item doesn't stamp. A DIRECTIONAL berm (bigger
// ahead of movement) models material plowed forward. Sampled by snow_displace.glsl
// (SnowDeformPress: uv = worldXZ / kWorld, REPEAT wrap, windowed to ±kHalf of the eye).

layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant) uniform PC {
    mat4 rainVP;      // world -> rain ndc (terrain-height gate)
    vec4 p0;          // x=kSize, y=decay (dt/life), z=kWorld (m), w=stampCount
    vec4 p1;          // xy=movement dir (world XZ), z=berm max (frac), w=ground gate (m)
    vec4 p2;          // x=rain eyeY, y=rain zRange, z=rough, w=unused
    vec4 p3;          // xy=cam window min (camXZ-kHalf), zw=prev cam window min
} pc;

layout(set = 0, binding = 0, r16f) uniform image2D uField;            // press field (read+write, in place)
layout(set = 0, binding = 2) uniform Stamps {
    vec4 sPos[64];   // xyz = world pos, w = radius (m)
    vec4 sPar[64];   // x = press strength (1 foot .. ~0.34 item), yz = facing (boot dir; 0,0 = round)
};
layout(set = 0, binding = 3) uniform sampler2D uRain;                 // terrain height (rain ortho depth)

// Value noise for per-print / per-edge irregularity (so the trail isn't a perfect repeat).
float cHash(vec2 p) { p = fract(p * vec2(127.1, 311.7)); p += dot(p, p + 34.23); return fract(p.x * p.y); }
float cNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
    float a = cHash(i), b = cHash(i + vec2(1,0)), c = cHash(i + vec2(0,1)), d = cHash(i + vec2(1,1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Actual terrain world-Y at a world XZ (rain ortho: eyeY - zNear - d*zRange).
float terrainY(vec2 xz) {
    vec4 c  = pc.rainVP * vec4(xz.x, 0.0, xz.y, 1.0);
    vec2 uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    float d = textureLod(uRain, clamp(uv, vec2(0.0), vec2(1.0)), 0.0).r;
    return pc.p2.x - 1.0 - d * pc.p2.y;
}

void main() {
    int   S    = int(pc.p0.x + 0.5);
    ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
    if (cell.x >= S || cell.y >= S) return;

    // ---- TOROIDAL texel -> world. A texel encodes a world XZ up to a kWorld period; pick
    // the representative inside the camera's ±kHalf window. wxz stays FIXED per texel until
    // the window edge scrolls past it, at which point it jumps by kWorld (= a new world
    // point rotated in from behind) — that flip is the "entered" test that clears it.
    const float kWorld = pc.p0.z;
    vec2 phase  = ((vec2(cell) + 0.5) / pc.p0.x - 0.5) * kWorld;      // world XZ mod kWorld (centred)
    vec2 camMin = pc.p3.xy;
    vec2 wxz    = camMin      + mod(phase - camMin,      vec2(kWorld));
    vec2 wprev  = pc.p3.zw    + mod(phase - pc.p3.zw,    vec2(kWorld));
    bool entered = (abs(wxz.x - wprev.x) > kWorld * 0.5) || (abs(wxz.y - wprev.y) > kWorld * 0.5);

    // Newly-scrolled-in texels start clean; the rest keep their stored press and decay it.
    float prev = entered ? 0.0 : imageLoad(uField, cell).r;
    const float dec = pc.p0.y;
    prev = (prev > 0.0) ? max(prev - dec, 0.0) : min(prev + dec, 0.0);

    float dent = max(prev, 0.0);     // + dent (deepest wins)
    float berm = min(prev, 0.0);     // − berm (highest pile wins)
    vec2  md   = pc.p1.xy;
    float bermMax = pc.p1.z;
    float gate    = pc.p1.w;
    bool  moving  = dot(md, md) > 1e-4;
    float rough = clamp(pc.p2.z, 0.0, 1.0);                      // r_snow_rough: trail imperfection
    int sc = int(pc.p0.w + 0.5);
    for (int i = 0; i < sc; ++i) {
        vec4  sp = sPos[i];
        float r  = max(sp.w, 0.05);
        vec2  o = wxz - sp.xz;
        float d = length(o);
        // Conservative reach cull BEFORE the noise. rEff <= r*1.4 (worst-case rough/wv), so
        // nothing beyond r*2.8 can be touched by this stamp. This skips the two cNoise taps
        // for the ~4M texels outside every stamp — they used to run for ALL texels (the
        // dominant cost); now only the ~1k texels near a stamp pay them. Output is identical
        // (the exact rEff*2 reach test still runs below).
        if (d > r * 2.8) continue;
        // Per-PRINT variation (noise keyed by the stamp position): the whole print is a
        // bit wider/narrower + deeper/shallower + its berm higher/lower -> no ctrl-c look.
        float wv     = cNoise(sp.xz * 0.8);
        float dv     = cNoise(sp.xz * 0.8 + vec2(19.3, 7.1));
        float rEff   = r * mix(1.0, 0.60 + 0.80 * wv, rough);
        float pressV = mix(1.0, 0.55 + 0.85 * dv, rough);
        if (d > rEff * 2.0) continue;                            // exact reach (after noise)
        if (abs(sp.y - terrainY(sp.xz)) > gate) continue;        // GROUND GATE: skip airborne/lifted
        float press = sPar[i].x * pressV;
        // Per-TEXEL rim wobble (2 octaves -> coarse waviness + fine ragged edge) plus a
        // directional bias so one side is rougher: imperfect walls + the odd "collapse".
        float wob = (cNoise(wxz * 2.6) - 0.5) * 0.7
                  + (cNoise(wxz * 6.5) - 0.5) * 0.35
                  + (cNoise(wxz * 1.3 + vec2(3.7, 9.1)) - 0.5) * 0.5;

        vec2 fdir = sPar[i].yz;
        if (dot(fdir, fdir) > 0.25) {
            // ---- BOOT print (RDR2-style): an oriented rounded-rect SOLE in the foot
            // frame with a TREAD (transverse lug bars), a deeper HEEL block and a
            // shallow ARCH between heel and forefoot. The rounded-rect SDF also drives
            // the displaced-soil berm right against the sole edge.
            vec2  fwd  = normalize(fdir);
            vec2  sideV = vec2(-fwd.y, fwd.x);
            float u = dot(o, fwd), v = dot(o, sideV);
            float halfL = rEff * 0.72, halfW = rEff * 0.30;      // ~30x13 cm boot at r=0.22
            float rc = halfW * 0.7;                              // rounded toe/heel corners
            vec2  q  = abs(vec2(u, v)) - vec2(halfL, halfW) + rc;
            float sd = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - rc
                     + wob * rough * 0.035;                      // ragged sole edge
            if (sd < 0.0) {
                float shape = 1.0 - smoothstep(-0.018, 0.0, sd); // sharp-ish wall, soft floor
                // Tread: lug bars across the sole (deep groove between raised soil strips)
                float bars = 0.78 + 0.22 * cos(u * 6.2831853 / 0.075);
                // Heel deeper (weight lands there), arch barely touches.
                float arch = mix(0.35, 1.0, smoothstep(0.06 * halfL, 0.30 * halfL, abs(u + 0.12 * halfL)));
                float heel = 1.0 + 0.15 * smoothstep(-0.35 * halfL, -0.75 * halfL, u);
                dent = max(dent, press * shape * bars * arch * heel);
            } else {
                // Berm: soil squeezed out around the sole; bigger ahead of movement.
                float dirW  = moving ? dot(o / max(d, 1e-4), md) : 0.0;
                float scale = mix(0.45, 1.4, clamp(dirW * 0.5 + 0.5, 0.0, 1.0)) * mix(1.0, 0.55 + 0.9 * wv, rough);
                float reach = rEff * 0.38 * (1.0 + 0.5 * max(dirW, 0.0));
                if (sd < reach) {
                    float hump = sin(clamp(sd / reach, 0.0, 1.0) * 3.14159265);
                    berm = min(berm, -hump * bermMax * scale * press);
                }
            }
        } else {
            // ---- ROUND contact (dropped items / legacy): cosine-bell basin + ring berm.
            float nd = d / rEff + wob * rough;
            nd = max(nd, 0.0);
            if (nd < 1.0) {
                float bell = 0.5 + 0.5 * cos(3.14159265 * nd);   // 1 centre .. 0 rim
                dent = max(dent, press * bell);
            } else {
                float dirW  = moving ? dot(o / max(d, 1e-4), md) : 0.0;
                float scale = mix(0.40, 1.5, clamp(dirW * 0.5 + 0.5, 0.0, 1.0)) * mix(1.0, 0.55 + 0.9 * wv, rough);
                float rOutN = 1.22 + 0.55 * max(dirW, 0.0);      // berm reach (normalised)
                if (nd < rOutN) {
                    float t    = (nd - 1.0) / max(rOutN - 1.0, 1e-4);
                    float hump = sin(t * 3.14159265);
                    berm = min(berm, -hump * bermMax * scale * press);
                }
            }
        }
    }
    float press = (dent > 0.0) ? dent : berm;
    imageStore(uField, cell, vec4(press, 0.0, 0.0, 0.0));
}
