#version 450
#extension GL_GOOGLE_include_directive : require
#include "froxel.glsl"   // Froxel_SliceFromViewZ — shared exp-Z mapping

// xrRenderVulkan — GPU particles #6 (drop CPU double-sim): SMOKE MEDIA SPLAT
// straight from the GPU particle pool.
//
// The CPU path feeds the froxel fog by walking PAPI particles into a
// SmokeParticle[] upload (CollectSmokeParticles → vol_splat). GPU-routed smoke
// no longer has CPU particles, so this pass reads the pool's WORLD-ALPHA alive
// region — exactly the PBM_BLEND smoke that qualifies as media (additive fire
// is self-emissive, HUD uses another projection; both live in other regions) —
// and atomic-adds into the SAME accum SSBO vol_splat uses, between Vol's clear
// and resolve. It reads LAST frame's aliveList/counters (this runs in the World
// pass, before this frame's gp_reset/sim) — a one-frame media lag, invisible.
//
// Froxel mapping + footprint deposit mirror vol_splat.comp verbatim; the
// two-zone distance LOD (full quality inside distFull, linear fade to maxD)
// mirrors CollectSmokeParticles so CPU and GPU smoke feed the fog identically.

layout(local_size_x = 64) in;

// Must match gp_common.glsl GPUParticle (64 B std430).
struct GPUParticle {
    vec4  pos_age;      // xyz = position, w = age (age < 0 → dead)
    vec4  vel_life;     // xyz = velocity, w = lifetime
    uint  colorRGBA;    // B8G8R8A8
    float rot;
    vec2  size;
    uint  defId;
    float origin_x, origin_y, origin_z;
};

layout(set = 0, binding = 0) readonly buffer PoolBuf     { GPUParticle pool[]; };
layout(set = 0, binding = 1) readonly buffer CountersBuf { uint counters[]; };  // [1] = world-alpha alive
layout(set = 0, binding = 2) readonly buffer AliveBuf    { uint aliveList[]; };
layout(set = 0, binding = 3)          buffer Accum       { uint accum[]; };     // Vol smoke accum [cell*4 + {d,R,G,B}]

// Same 112 B layout as vol_splat's Push; the unused .w lanes carry the LOD zone.
layout(push_constant) uniform Push {
    vec4 camPos;     // xyz world camera pos;                w = distFull (full-quality radius)
    vec4 camDir;     // xyz camera forward (unit);           w = maxD (media cutoff)
    vec4 camRightT;  // xyz right * tan(fovX/2)
    vec4 camTopT;    // xyz top   * tan(fovY/2)
    vec4 zParams;    // x=near y=far z=log2(far/near)
    vec4 dims;       // xyz = smoke grid dims, w = fixed-point scale
    vec4 params;     // x = pool capacity (bounds guard), y = density scale, z = max footprint (cells)
} pc;

#include "splat_common.glsl"   // depositCell + splatFootprint — shared with vol_splat

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    uint cnt = min(counters[1], uint(pc.params.x));   // world-alpha region [0, cnt)
    if (gid >= cnt) return;

    GPUParticle p = pool[aliveList[gid]];
    if (p.pos_age.w < 0.0) return;                    // defensive: dead slot

    vec4 col4 = vec4(float((p.colorRGBA >> 16) & 0xFFu),
                     float((p.colorRGBA >> 8)  & 0xFFu),
                     float( p.colorRGBA        & 0xFFu),
                     float((p.colorRGBA >> 24) & 0xFFu)) / 255.0;
    if (col4.a <= 0.003) return;                      // invisible → no media

    vec3  rel   = p.pos_age.xyz - pc.camPos.xyz;
    float nearZ = pc.zParams.x;

    float viewZ = dot(rel, pc.camDir.xyz);
    if (viewZ <= nearZ || viewZ >= pc.zParams.y) return;

    // Two-zone LOD (mirrors CollectSmokeParticles): full density inside distFull,
    // linear fade to zero by maxD — beyond that the billboard alone represents it.
    float distFull = pc.camPos.w;
    float maxD     = pc.camDir.w;
    float dd = length(rel);
    if (dd > maxD) return;
    float dens = col4.a;
    if (dd > distFull) dens *= max(1.0 - (dd - distFull) / max(maxD - distFull, 0.001), 0.0);
    if (dens <= 0.003) return;
    dens *= pc.params.y;

    float ndcx = dot(rel, pc.camRightT.xyz) / (viewZ * dot(pc.camRightT.xyz, pc.camRightT.xyz));
    float ndcy = dot(rel, pc.camTopT.xyz)   / (viewZ * dot(pc.camTopT.xyz,   pc.camTopT.xyz));
    if (abs(ndcx) > 1.0 || abs(ndcy) > 1.0) return;

    vec2  uv = vec2(ndcx * 0.5 + 0.5, (1.0 - ndcy) * 0.5);
    float w  = Froxel_SliceFromViewZ(viewZ, nearZ, pc.zParams.z);

    ivec3 dim = ivec3(pc.dims.xyz);
    vec3  col = max(col4.rgb, vec3(0.0));

    vec3  fc = vec3(uv.x * float(dim.x), uv.y * float(dim.y), w * float(dim.z));

    // Same footprint rule as vol_splat — only the radius source differs (a GPU
    // particle has a billboard size, not a stored world radius).
    float radius    = (p.size.x + p.size.y) * 0.25;   // avg billboard half-extent
    float cellWorld = 2.0 * length(pc.camRightT.xyz) * viewZ / float(dim.x);
    int   fr = min(int(radius / max(cellWorld, 1e-3)), int(pc.params.z));

    splatFootprint(fc, dim, fr, dens, col);
}
