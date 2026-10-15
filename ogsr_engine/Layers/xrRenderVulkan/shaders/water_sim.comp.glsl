#version 450
// xrRenderVulkan - Water flow simulation (compute) — VELOCITY / momentum model.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
// SPDX-License-Identifier: MIT
//
// Shallow water with MOMENTUM on the rain-occlusion ortho grid (same VP/res as
// the rain map). State = water DEPTH (R16F) + VELOCITY (RG16F, uv-units/sec).
// Per frame (1 step):
//   1. reproject prev depth+velocity through the previous VP (the box moves with
//      the camera, texel-snapped → translation only, velocity vector unchanged);
//   2. accelerate velocity DOWNHILL by the gradient of the surface (ground+water)
//      + friction damping → water gains inertia and runs in channels/streams,
//      pours off roof edges (roofs are in the height field);
//   3. ADVECT depth by the velocity (semi-Lagrangian backtrace — unconditionally
//      stable); + rain where exposed to sky, − exponential leak (self-limiting).
// Ground height = the clean ground map (no trees); exposure = ground≈rain map
// (under trees/roofs the rain map sits higher → no rain there).

layout(local_size_x = 8, local_size_y = 8) in;

layout(push_constant) uniform PC {
    mat4  curInvVP;   // cell ndc -> world (current), for reprojection
    mat4  prevVP;     // world -> previous ndc
    vec4  p0;         // x=N, y=rainAdd·dt, z=leak·dt, w=accel (gravity·dt²)
    vec4  p1;         // x=rainDensity, y=zNear, z=zRange, w=hasPrev
    vec4  p2;         // x=damping, y=dt, z=maxVel(uv/s), w=unused
} pc;

layout(set = 0, binding = 0) uniform sampler2D uTerrain;  // ground height (ortho depth, no trees)
layout(set = 0, binding = 1) uniform sampler2D uRain;     // rain occlusion (topmost incl trees) → exposure
layout(set = 0, binding = 2) uniform sampler2D uDepthP;   // prev water depth
layout(set = 0, binding = 3) uniform sampler2D uVelP;     // prev velocity (uv/sec)
layout(set = 0, binding = 4, r16f)  writeonly uniform image2D uDepthOut;
layout(set = 0, binding = 5, rg16f) writeonly uniform image2D uVelOut;

float terrainM(vec2 uv) { return -(pc.p1.y + textureLod(uTerrain, uv, 0.0).r * pc.p1.z); }   // metres, up+
float rainM   (vec2 uv) { return -(pc.p1.y + textureLod(uRain,    uv, 0.0).r * pc.p1.z); }

vec2 cellWorldXZ(vec2 uv)
{
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
    vec4 w   = pc.curInvVP * vec4(ndc, 0.0, 1.0);
    return w.xz / w.w;
}
// current cell uv -> previous-frame uv (advect the box motion)
vec2 toPrevUV(vec2 uv)
{
    if (pc.p1.w < 0.5) return uv;
    vec2 wxz = cellWorldXZ(uv);
    vec4 c   = pc.prevVP * vec4(wxz.x, 0.0, wxz.y, 1.0);
    vec2 p   = c.xy / c.w * 0.5 + 0.5; p.y = 1.0 - p.y;
    return p;
}
float depthPrevAt(vec2 uv)
{
    vec2 p = toPrevUV(uv);
    if (any(lessThan(p, vec2(0.0))) || any(greaterThan(p, vec2(1.0)))) return 0.0;
    return textureLod(uDepthP, p, 0.0).r;
}

void main()
{
    int N = int(pc.p0.x + 0.5);
    ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
    if (cell.x >= N || cell.y >= N) return;

    float texel = 1.0 / float(N);
    vec2  uv    = (vec2(cell) + 0.5) * texel;

    // Reproject previous velocity (vector unchanged under box translation).
    vec2 v = vec2(0.0);
    if (pc.p1.w > 0.5) {
        vec2 pv = toPrevUV(uv);
        if (all(greaterThanEqual(pv, vec2(0.0))) && all(lessThanEqual(pv, vec2(1.0))))
            v = textureLod(uVelP, pv, 0.0).rg;
    }

    // Surface = ground + water (use reprojected prev depth). Downhill gradient
    // drives the flow; central differences in uv.
    float H  = terrainM(uv);
    float d0 = depthPrevAt(uv);
    float sC = H + d0;
    float sR = terrainM(uv + vec2(texel, 0.0)) + depthPrevAt(uv + vec2(texel, 0.0));
    float sL = terrainM(uv - vec2(texel, 0.0)) + depthPrevAt(uv - vec2(texel, 0.0));
    float sT = terrainM(uv + vec2(0.0, texel)) + depthPrevAt(uv + vec2(0.0, texel));
    float sB = terrainM(uv - vec2(0.0, texel)) + depthPrevAt(uv - vec2(0.0, texel));
    vec2 grad = vec2(sR - sL, sT - sB) * 0.5;       // metres per texel

    // Accelerate downhill (−grad) + friction damping; cap speed for stability.
    v = v * pc.p2.x - grad * pc.p0.w;
    float sp = length(v);
    if (sp > pc.p2.z) v *= pc.p2.z / sp;
    // Water only flows where there IS water (a dry cell has no momentum to give).
    if (d0 < 0.0005) v = vec2(0.0);

    // Advect depth: semi-Lagrangian backtrace along velocity (stable). Sample the
    // prev depth one step "upstream" (and reproject for box motion).
    vec2 srcUV = uv - v * pc.p2.y;
    float d = depthPrevAt(srcUV);

    // Rain input where exposed to the sky (ground ≈ rain-map topmost; under a
    // tree/roof the rain map sits higher → no rain reaches this ground cell).
    float exposure = step(rainM(uv) - H, 0.25);     // 0.25 m tolerance
    d += pc.p0.y * pc.p1.x * exposure;

    // Exponential leak (self-limiting drain): flats settle shallow, dips deeper.
    d *= max(0.0, 1.0 - pc.p0.z);
    d = clamp(d, 0.0, 5.0);

    imageStore(uDepthOut, cell, vec4(d, 0.0, 0.0, 0.0));
    imageStore(uVelOut,   cell, vec4(v, 0.0, 0.0));
}
