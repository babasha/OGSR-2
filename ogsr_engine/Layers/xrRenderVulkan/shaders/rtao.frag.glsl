#version 460
// GL_EXT_ray_query needs GLSL 460 — under 450 glslang does not even declare
// `rayQueryEXT` (the rest of this renderer's shaders are 450; this one has to
// be the exception, not a style slip).
#extension GL_EXT_ray_query : require
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — hardware ray-traced ambient occlusion (RTAO).
//
// Runs at the SAME half resolution, in the SAME slot and into the SAME target as
// GTAO (vk_pass_ssao): the pass draws GTAO first, then this shader min-blends
// over the raw AO attachment. That is deliberate, not laziness:
//
//   * every consumer downstream (blur, temporal EMA, EnvLight binding 8) keeps
//     working untouched — RTAO is a source of AO, not a new pipeline;
//   * min() keeps the screen-space result wherever the ray tracer has nothing to
//     say. The BVH holds STATIC collision geometry only, so NPCs, weapons and
//     doors still occlude through GTAO. Whichever pass sees an occluder wins.
//
// The rays are traced against a camera-local BLAS cut out of level.cform (see
// vk_rt_scene.h) — collision, not render, geometry. It is world-space, LOD-free
// and already resident, and it carries the "collision only" flag so invisible
// walls can be dropped. Beyond the bubble the result fades back to open sky.
//
// THREE RULES THIS PASS FOLLOWS THAT v1 DID NOT (06-09, "RTAO adds too much black"):
//
//   1. OPAQUE WORLD ONLY (instance mask 0x01). The TLAS carries a second instance,
//      0x02, of alpha-tested cutouts — every leaf card, fence and grate. v1 traced
//      it with RayFlagsOpaque, which commits every card WITHOUT its alpha test: a
//      tree crown was a solid ball, a chain-link fence a wall. In a forest that
//      structure is 99.5% of the triangles (1.27 M cutout vs 6.8 k opaque, see
//      vk_console_min r_rt_tree_range) and it painted everything near foliage
//      black. The alpha test the GI rays run (rt_shade.glsl rtOccluded) costs half
//      a ray; AO does not need it — GTAO still shades the foliage through the min.
//   2. HIT DISTANCE FALLOFF. Every hit inside the radius counted as full occlusion,
//      so a floor darkened in a flat 2 m band along every wall. Occlusion is now
//      weighted (1 - t / R)^k (r_rtao_falloff), the obscurance kernel production
//      RTAO uses (NVIDIA RTAO, UE, Frostbite) — NEAREST hit, so no
//      TerminateOnFirstHit any more.
//   3. NO DOUBLE EXPONENT. Consumers read the AO buffer as pow(ao, r_ssao_strength),
//      strength 2 by default — tuned for GTAO's soft horizon integral. A cosine-
//      weighted traced visibility is ALREADY the integral; squaring it turned 0.5
//      into 0.25. The value written is pre-compensated by pow(ao, 1/strength) so
//      what the consumer sees after its exponent is the traced term itself (min is
//      monotone, so the blend with GTAO survives the trick).
//   Plus: a hit on a BACK face (rayQueryGetIntersectionFrontFaceEXT false) is a
//   ray that started inside geometry — the origin comes from half-res depth plus a
//   fixed lift — and counts as a miss (r_rtao_backface). Render-geometry winding
//   is dependable (rt_shade.glsl h.back); the collision soup's is not, so the
//   flag is only honoured for r_rt_src 1 (vk_pass_rtao.cpp zeroes it otherwise).
//
// Output: R16F visibility (1 = open, 0 = fully occluded), MRT target 0. Target 1
// (SSIL) is attached because the pipeline shares GTAO's attachment layout; its
// write mask is 0, so what this shader stores there is discarded.

layout(location = 0) out vec4 outAO;
layout(location = 1) out vec4 outIL;

layout(set = 0, binding = 0) uniform sampler2D uDepth;    // full-res prepass depth (D32)
layout(set = 0, binding = 1) uniform sampler2D uNormal;   // normal G-buffer: rgb = worldN*0.5+0.5, a = source tag (1 skinned, 0.5 static, 0 none)
layout(set = 0, binding = 2) uniform accelerationStructureEXT uTLAS;

layout(push_constant) uniform PC {
    vec4 camDir;     // xyz = camera forward (unit); w = ray length (m)
    vec4 camRightT;  // xyz = right * tan(fovX/2), w = tan(fovX/2)
    vec4 camTopT;    // xyz = top   * tan(fovY/2), w = tan(fovY/2)
    vec4 zp;         // x = proj _33, y = proj _43, z = rays per pixel, w = normal-offset bias (m)
    vec4 res;        // xy = AO target size, zw = 1 / AO target size
    vec4 camPos;     // xyz = camera world position, w = RTAO cutoff distance (m)
    vec4 params;     // x = per-frame jitter phase [0,1), y = AO power, z = unused, w = debug
    vec4 shape;      // x = distance-falloff exponent (0 = binary hit), y = 1 / consumer strength exponent
                     // (pre-compensation, see rule 3), z = backface hit counts as a miss (1) or a hit (0), w = unused
} pc;

const float PI = 3.14159265;

// Position + normal reconstruction is SHARED with the final gather
// (shaders/gbuffer_recon.glsl): both start every ray from this answer, and two
// copies would drift into looking like a bug in whichever was read second.
#include "gbuffer_recon.glsl"

// Interleaved-gradient noise (Jimenez) — spreads a rotation well over a pixel
// neighbourhood, which is exactly what the depth-aware blur downstream averages.
// The per-frame phase rotates it so the temporal EMA in ssao_blur averages a
// DIFFERENT ray set each frame instead of re-averaging the same one.
float ign(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Plain white-noise hash (Hugo Elias / "hash without sine"). Used for the SECOND
// sampling dimension only: IGN's gradient is DIAGONAL, so driving both the
// azimuth and the radius from it locks them together and prints that diagonal
// straight into the AO — the corduroy hatching r_rtao_debug showed at 2 rays.
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// van der Corput / radical inverse base 2 — stratifies the elevation across the
// ray budget, so 2 rays cover two different elevations instead of landing at the
// same one twice.
float radicalInverse(int i)
{
    uint b = uint(i);
    b = (b << 16u) | (b >> 16u);
    b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
    b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
    b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
    b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
    return float(b) * 2.3283064365386963e-10;
}

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.res.zw;

    float zndc = texture(uDepth, uv).r;
    if (zndc >= 0.9999) { outAO = vec4(1.0); outIL = vec4(0.0); return; }   // sky / no prepass coverage

    vec4  C     = fetchPos(uv);
    vec3  cPos  = C.xyz;                 // camera-RELATIVE world position
    float zview = C.w;

    // Past the cutoff the BLAS bubble no longer covers the geometry, so a miss
    // would mean "nothing here" instead of "nothing built" — fade out first.
    float cutoff = pc.camPos.w;
    float fade   = 1.0 - smoothstep(cutoff * 0.75, cutoff, zview);
    if (fade <= 0.001) { outAO = vec4(1.0); outIL = vec4(0.0); return; }

    vec3 viewV = -normalize(cPos);

    vec3 N = reconNormal(uv, C, viewV);

    // World-space ray origin, lifted off the surface. The BVH is COLLISION
    // geometry, which does not sit exactly on the rendered surface (and the
    // reconstructed position carries depth quantization), so the offset has to
    // cover both — a fixed bias plus a distance term for the far, coarse pixels.
    vec3 P = pc.camPos.xyz + cPos + N * (pc.zp.w + zview * 0.002);

    // Tangent frame for the cosine-weighted hemisphere.
    vec3 up = abs(N.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(up, N));
    vec3 B  = cross(N, T);

    int   rays = int(pc.zp.z);
    float tmax = pc.camDir.w;

    // TWO INDEPENDENT offsets, one per sampling dimension (see hash12 above for
    // why sharing one was wrong). Both advance with the per-frame phase so the
    // temporal EMA keeps averaging a genuinely new set.
    float rot = fract(ign(gl_FragCoord.xy) + pc.params.x);
    float rad = fract(hash12(gl_FragCoord.xy) + pc.params.x * 0.7548776662);

    float occ = 0.0;
    const float kFall      = pc.shape.x;
    const bool  backIsMiss = pc.shape.z > 0.5;
    // 0x01 = the opaque static world; 0x04 = this frame's skeletons (vk_rt_dyn:
    // NPCs, weapons, doors, props) when the bound TLAS carries them (shape.w).
    const uint  mask       = 0x01u | ((pc.shape.w > 0.5) ? 0x04u : 0u);
    for (int i = 0; i < rays; ++i) {
        // Cosine-weighted and stratified in BOTH dimensions: azimuth by ray index
        // (2 rays = two opposite sectors), elevation by the radical inverse. The
        // old scheme stratified only the radius and rotated the azimuth by the
        // golden ratio off the SAME noise, so at 2 rays the pair could sit almost
        // on top of each other.
        float phi = 2.0 * PI * ((float(i) + rot) / float(rays));
        float u   = fract(radicalInverse(i) + rad);
        float r   = sqrt(u);
        vec3  dir = normalize(T * (r * cos(phi)) + B * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - u)));

        // Mask 0x01 = the opaque structure only (rule 1). RayFlagsOpaque is then
        // exact rather than a shortcut: nothing alpha-tested can reach the query.
        // NEAREST hit (no TerminateOnFirstHit): the weight below reads the distance.
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, uTLAS, gl_RayFlagsOpaqueEXT, mask, P, 0.01, dir, tmax);
        while (rayQueryProceedEXT(rq)) {}
        if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
            // The back-face rule holds for the render world's winding; the dynamic
            // instance (custom index 0xFFFFFF, see vk_rt_dyn) is drawn without
            // culling and its winding is not a promise — every hit on it counts.
            const bool onDyn = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) == 0xFFFFFF;
            if (backIsMiss && !onDyn && !rayQueryGetIntersectionFrontFaceEXT(rq, true)) continue;
            float t = rayQueryGetIntersectionTEXT(rq, true);
            // (1 - t/R)^k: k = 0 is the old binary hit, 1 the linear obscurance
            // kernel, 2 a softer one that keeps only real contact (rule 2).
            occ += (kFall > 0.0) ? pow(clamp(1.0 - t / tmax, 0.0, 1.0), kFall) : 1.0;
        }
    }

    // Cosine-weighted rays make the plain average the visibility integral itself.
    float ao = 1.0 - occ / float(rays);
    ao = pow(clamp(ao, 0.0, 1.0), max(0.05, pc.params.y));
    ao = mix(1.0, ao, fade);
    // Rule 3: undo the consumer's strength exponent in advance (shape.y = 1/strength).
    ao = pow(ao, pc.shape.y);

    outAO = vec4(ao, 0.0, 0.0, 1.0);
    outIL = vec4(0.0);
}
