// Shared structs/bindings for GPU-driven particles — Phase 1 (proof of life).
// Must match VK::GPUParticles::Particle (64 B, std430).
//
// One hardcoded effect (Source + Gravity + KillOld) lives entirely on the GPU:
//   gp_init  — one-time: mark whole pool dead, fill the free-list, set counters.
//   gp_reset — per-frame: aliveCount = 0.
//   gp_emit  — pop free slots (atomic), spawn hardcoded smoke/dust particles.
//   gp_sim   — integrate; KillOld pushes the slot back to the free-list;
//              survivors append themselves to aliveList.
//   gp_build — write the indirect draw (instanceCount = aliveCount).
//
// Phase 2+ (action-program interpreter, spawn requests, sort) was intentionally
// cut — see gpu_particles_roadmap.md.

struct GPUParticle {
    vec4  pos_age;      // xyz = position, w = age (age < 0 → dead)
    vec4  vel_life;     // xyz = velocity, w = lifetime (seconds)
    uint  colorRGBA;    // B8G8R8A8 (matches gp_particle.vert unpackBGRA8)
    float rot;
    vec2  size;
    uint  defId;        // per-particle program (registry index)
    // Emitter world spawn position (repurposed from the old `flags`+padding).
    // Positional force fields build a world centre = origin + local-centre.
    float origin_x, origin_y, origin_z;
};

// ----- Phase 2: action-program interpreter --------------------------------
// CPU parses a .pe (or, for now, an authored effect) into a GPU program:
//   * EmitDesc  — the Source action resolved into domains the emit shader
//                 samples (position/velocity/size/color/rotation + scalars).
//   * ActionRec[] — the simulate-time actions (Gravity/Move/Damping/…),
//                 interpreted per-particle in gp_simulate, in list order.
// Layout must match VK::GPUParticles::{GenDomain,ActionRec,EmitDesc,Program}.

// One pDomain resolved for GPU sampling (mirrors PAPI::pDomain essentials).
struct GenDomain {
    uint  dtype;            // PDomainEnum (PDPoint=0 … PDRectangle=10)
    uint  _gp0, _gp1, _gp2;
    vec4  p1;               // point / box-min / sphere-centre / line-start …
    vec4  p2;               // box-max / line-delta / cyl axis vector …
    vec4  du;               // 'u' orthonormal basis (cyl/cone/disc/rect)
    vec4  dv;               // 'v' orthonormal basis
    vec4  radii;            // radius1, radius2, radius1Sqr, radius2Sqr
};

// One simulate-time action. `atype` = PActionEnum; a/b/c hold its params.
struct ActionRec {
    uint  atype;
    uint  _ap0, _ap1, _ap2;
    vec4  a;
    vec4  b;
    vec4  c;
};

// Source action resolved for the emit shader.
struct EmitDesc {
    GenDomain posDom;
    GenDomain velDom;
    GenDomain sizeDom;
    GenDomain colorDom;
    GenDomain rotDom;
    vec4  sc;              // x=alpha, y=age, z=age_sigma, w=lifetime
    vec4  parentVel;       // xyz = inherited emitter velocity
};

// PActionEnum ids we interpret (see psystem.h). Unlisted actions are no-ops.
// Tier-A (Phase 2a):
const uint GP_DAMPING        = 4u;
const uint GP_GRAVITY        = 8u;
const uint GP_KILLOLD        = 10u;
const uint GP_MOVE           = 12u;
const uint GP_SPEEDLIMIT     = 22u;
const uint GP_TARGETCOLOR    = 23u;
const uint GP_TARGETSIZE     = 24u;
const uint GP_TARGETVELOCITY = 27u;
// Tier-B/C (Phase 3 step #1 — full interpreter). Force fields + random walks +
// turbulence. Domain-based actions (Random*) pack a REDUCED pDomain into the
// ActionRec (dtype in _ap0, p1 in a.xyz, p2 in b.xyz, radii in c.xy) — see
// gp_genCompact; the force/rotate actions pack scalar params into a/b/c.
const uint GP_ORBITLINE      = 13u;
const uint GP_ORBITPOINT     = 14u;
const uint GP_RANDOMACCEL    = 15u;
const uint GP_RANDOMDISPLACE = 16u;
const uint GP_RANDOMVELOCITY = 17u;
const uint GP_TARGETROTATE    = 25u;   // PATargetRotateD (26) is normalised to this
const uint GP_VORTEX         = 29u;
const uint GP_TURBULENCE     = 30u;
const uint GP_SCATTER        = 31u;

// One full effect program (fixed size so programs can live in an array — Phase 3
// step 1, per-defId registry). Must match VK::GPUParticles::Program (2576 B).
#define GP_MAX_ACTIONS 32
struct Program {
    uint      actionCount;
    float     emitRate;     // particles/sec (CPU also uses this; reserved here)
    uint      progFlags;
    uint      _prog_pad;
    EmitDesc  emit;
    ActionRec actions[GP_MAX_ACTIONS];
};

// One emit request (Phase 3 step 2). CPU appends one per active emitter per
// frame; gp_emit walks them so a single dispatch services many emitters, each
// with its own program + world position. Owns emit gids [firstSlot,firstSlot+count).
struct SpawnRequest {       // 48 B std430
    vec4  pos;              // xyz = world spawn position; w = per-program alive cap (uint bits, 0 = uncapped)
    vec4  vel;              // xyz = inherited parent velocity
    uint  program;         // program index (becomes particle.defId)
    uint  count;           // particles this request spawns this frame
    uint  firstSlot;       // base emit-gid this request owns
    uint  seed;            // RNG seed base for this request
};

// SSBOs — same binding layout for every dispatch + the draw.
//   counters[0] = freeCount (top of the free-list stack; persistent)
//   counters[1..4] = per-group alive counts this frame (reset by gp_reset):
//     [1]=world-alpha [2]=world-add [3]=hud-alpha [4]=hud-add
layout(set = 0, binding = 0) buffer PoolBuf     { GPUParticle pool[]; };
layout(set = 0, binding = 1) buffer CountersBuf { uint counters[]; };
layout(set = 0, binding = 2) buffer AliveBuf    { uint aliveList[]; };
layout(set = 0, binding = 3) buffer IndirectBuf { uint drawCmd[]; };
layout(set = 0, binding = 4) buffer FreeBuf     { uint freeList[]; };
// Per-defId program registry: each live particle picks programs[p.defId];
// gp_emit spawns from the program named by its spawn request.
layout(set = 0, binding = 5) buffer ProgramBuf  { Program programs[]; };
// Per-frame spawn requests (one per active emitter) — written by the CPU.
layout(set = 0, binding = 6) buffer SpawnReqBuf { SpawnRequest spawnReqs[]; };
// #5 per-program alive counts (persistent, zeroed by gp_init): gp_emit claims
// against the request's cap, gp_simulate releases on death. Enforces
// cap = m_MaxParticles × instances so one greedy effect can't starve the pool.
#define GP_MAX_PROGRAMS 64
layout(set = 0, binding = 8) buffer ProgAliveBuf { uint progAlive[]; };
// Phase 4 alpha sort scratch: [0..GP_SORT_BUCKETS) = depth-bucket histogram /
// scan offsets (zeroed by gp_reset), [GP_SORT_BUCKETS..) = a copy of this
// frame's world-alpha aliveList entries (gp_sort_hist writes it, gp_sort_scatter
// reads it while rewriting aliveList[0..count) back-to-front).
#define GP_SORT_BUCKETS 512
layout(set = 0, binding = 9) buffer SortBuf { uint sortData[]; };
// Emitter kill requests (CPU parity: a hard Stop wipes the instance's
// particles instantly — PAPI p_count=0 — and destruction removes them with
// the object). gp_simulate culls particles matching program + spawn origin.
struct KillRequest {        // 32 B std430
    vec4  posRadius;        // xyz = emitter position, w = radius SQUARED
    uint  program;
    uint  _kp0, _kp1, _kp2;
};
layout(set = 0, binding = 10) buffer KillReqBuf { KillRequest killReqs[]; };

// Log-quantized depth bucket for the alpha sort. Bucket 0 = FARTHEST (drawn
// first), GP_SORT_BUCKETS-1 = nearest. Fixed [near0, far0] range — only bucket
// RESOLUTION depends on it, not correctness (out-of-range clamps).
uint gp_sortBucket(float viewZ)
{
    const float near0 = 0.75;
    const float far0  = 1800.0;
    float t = log2(max(viewZ, near0) / near0) / log2(far0 / near0);   // 0 near .. 1 far
    uint  b = uint(clamp(t, 0.0, 1.0) * float(GP_SORT_BUCKETS - 1) + 0.5);
    return (GP_SORT_BUCKETS - 1u) - b;                                // far -> bucket 0
}

// Push constants (compute) — must match VK::GPUParticles::ComputePush.
struct GPPush {
    vec4  spawnPos;       // xyz = emitter pos, w = emitCount (as float-bits uint)
    vec4  dt_gravity;     // x = dt, y = gravity magnitude (m/s^2)
    vec4  camPos;         // xyz = camera position (Phase 4 depth sort)
    vec4  camForward;     // xyz = camera forward (Phase 4 depth sort)
    uint  maxParticles;
    uint  frameSeed;
    uint  activeProgram;  // reserved (emit now reads program per spawn request)
    uint  numRequests;    // active spawn requests this frame
    uint  numKills;       // emitter kill requests this frame
};
layout(push_constant) uniform PC { GPPush pc; };

uint gp_emitCount() { return floatBitsToUint(pc.spawnPos.w); }

// Simple integer hash RNG.
uint gp_hash(uint x)
{
    x = (x ^ 61u) ^ (x >> 16u);
    x = x + (x << 3u);
    x = x ^ (x >> 4u);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}
float gp_frand(uint x) { return float(gp_hash(x)) / 4294967296.0; }

// ----- RNG stream (advances the seed; mirrors PAPI drand48 call order) -----
float gp_rand1(inout uint s)  { float r = gp_frand(s); s += 1u; return r; }
vec3  gp_rand3(inout uint s)  { vec3 r = vec3(gp_frand(s), gp_frand(s + 1u), gp_frand(s + 2u)); s += 3u; return r; }

#define GP_PI    3.14159265359
#define GP_2PI   6.28318530718
#define GP_MAXF  1.0e16

// Port of PAPI::pDomain::Generate — a uniform random point in the domain.
vec3 gp_generate(GenDomain d, inout uint s)
{
    switch (d.dtype) {
    case 0u:  // PDPoint
        return d.p1.xyz;
    case 1u:  // PDLine: p1 + p2*t   (p2 is the start→end delta)
        return d.p1.xyz + d.p2.xyz * gp_rand1(s);
    case 4u: {// PDBox: p1=min, p2=max
        vec3 t = gp_rand3(s);
        return d.p1.xyz + (d.p2.xyz - d.p1.xyz) * t;
    }
    case 5u: {// PDSphere shell [radius2..radius1]
        vec3 p = normalize(gp_rand3(s) - vec3(0.5) + vec3(1e-6));
        float r1 = d.radii.x, r2 = d.radii.y;
        float r = (r1 == r2) ? r1 : (r2 + gp_rand1(s) * (r1 - r2));
        return d.p1.xyz + p * r;
    }
    case 6u:   // PDCylinder
    case 7u: { // PDCone (apex at p1)
        float dist  = gp_rand1(s);
        float theta = gp_rand1(s) * GP_2PI;
        float r     = d.radii.y + gp_rand1(s) * (d.radii.x - d.radii.y);
        float x = r * cos(theta);
        float y = r * sin(theta);
        if (d.dtype == 7u) { x *= dist; y *= dist; }
        return d.p1.xyz + d.p2.xyz * dist + d.du.xyz * x + d.dv.xyz * y;
    }
    case 9u: { // PDDisc
        float theta = gp_rand1(s) * GP_2PI;
        float r     = d.radii.y + gp_rand1(s) * (d.radii.x - d.radii.y);
        return d.p1.xyz + d.du.xyz * (r * cos(theta)) + d.dv.xyz * (r * sin(theta));
    }
    case 10u:  // PDRectangle
        return d.p1.xyz + d.du.xyz * gp_rand1(s) + d.dv.xyz * gp_rand1(s);
    case 2u: { // PDTriangle
        float r1 = gp_rand1(s), r2 = gp_rand1(s);
        if (r1 + r2 < 1.0) return d.p1.xyz + d.du.xyz * r1 + d.dv.xyz * r2;
        return d.p1.xyz + d.du.xyz * (1.0 - r1) + d.dv.xyz * (1.0 - r2);
    }
    case 8u:   // PDBlob — approximate the gaussian with a box of width 2*sigma
        return d.p1.xyz + (gp_rand3(s) - vec3(0.5)) * (d.radii.x * 2.0);
    default:   // PDPlane (3) and anything else → the anchor point
        return d.p1.xyz;
    }
}

// Reduced-domain sampler for simulate-time Random{Velocity,Accel,Displace}.
// The action packs only dtype + p1 + p2 + radii (no u/v basis) into its
// a/b/c fields, so this covers the domains these actions actually use in
// content — Point/Line/Box/Sphere/Blob. Anything else falls back to p1.
vec3 gp_genCompact(uint dtype, vec3 p1, vec3 p2, vec2 radii, inout uint s)
{
    switch (dtype) {
    case 0u:  return p1;                                    // PDPoint
    case 1u:  return p1 + p2 * gp_rand1(s);                 // PDLine (p2 = delta)
    case 4u: { vec3 t = gp_rand3(s); return p1 + (p2 - p1) * t; }  // PDBox
    case 5u: {                                              // PDSphere shell
        vec3 d = normalize(gp_rand3(s) - vec3(0.5) + vec3(1e-6));
        float r1 = radii.x, r2 = radii.y;
        float r = (r1 == r2) ? r1 : (r2 + gp_rand1(s) * (r1 - r2));
        return p1 + d * r;
    }
    case 8u:  return p1 + (gp_rand3(s) - vec3(0.5)) * (radii.x * 2.0);  // PDBlob
    default:  return p1;
    }
}

// ----- 3D fractal noise (Turbulence) --------------------------------------
// Approximates PAPI noise.cpp fractalsum3 (Perlin fBm, lacunarity 2.059).
// A gradient-noise field is enough for the visual — the exact permutation
// table isn't reproduced; the GPU turbulence is a documented approximation.
float gp_vhash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float gp_noise3(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n = mix(mix(mix(gp_vhash(i + vec3(0,0,0)), gp_vhash(i + vec3(1,0,0)), f.x),
                      mix(gp_vhash(i + vec3(0,1,0)), gp_vhash(i + vec3(1,1,0)), f.x), f.y),
                  mix(mix(gp_vhash(i + vec3(0,0,1)), gp_vhash(i + vec3(1,0,1)), f.x),
                      mix(gp_vhash(i + vec3(0,1,1)), gp_vhash(i + vec3(1,1,1)), f.x), f.y), f.z);
    return n * 2.0 - 1.0;   // [0,1] -> [-1,1], matching noise3's range
}
float gp_fractalsum3(vec3 v, float freq, int octaves)
{
    float boost = freq;
    float f = freq;
    float sum = 0.0;
    for (int o = 0; o < octaves; ++o) {
        sum += gp_noise3(v * f) / f;
        f *= 2.059;
    }
    return sum * boost;
}

// Particle colour is stored packed B8G8R8A8 (matches gp_particle.vert).
vec4 gp_unpackBGRA(uint c)
{
    return vec4(float((c >> 16) & 0xFFu),
                float((c >> 8)  & 0xFFu),
                float( c        & 0xFFu),
                float((c >> 24) & 0xFFu)) / 255.0;
}
uint gp_packBGRA(vec4 col)
{
    col = clamp(col, 0.0, 1.0) * 255.0 + 0.5;
    return (uint(col.b)) | (uint(col.g) << 8) | (uint(col.r) << 16) | (uint(col.a) << 24);
}
