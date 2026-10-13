// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// GPU-Driven Particles — Phase 1 (proof of life). Roadmap: gpu_particles_roadmap.md
//
// One hardcoded effect (Source + Gravity + KillOld) lives entirely on the GPU:
// resident SSBOs, a GPU free-list for slot recycling, compute emit/simulate,
// and an indirect billboard draw. No per-frame vertex upload. Off by default
// (r_gpu_particles 0) — the legacy PAPI/billboard path still draws everything.
//
// Particle layout (std430, 64 B) — must match gp_common.glsl GPUParticle:
//   pos_age  = vec4 (xyz = pos, w = age; age < 0 → dead)
//   vel_life = vec4 (xyz = vel, w = lifetime)
//   colorRGBA(uint B8G8R8A8), rot(float), size(vec2), defId(uint), flags(uint)
#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

// Cvars are global (defined in vk_console_min.cpp); the .cpp declares them
// with file-scope `extern`. They must NOT be declared inside the namespace
// below, or unqualified lookup binds to a non-existent namespace member.

namespace VK { namespace GPUParticles {

// Must match gp_common.glsl GPUParticle. The former `flags` (unused) + the
// struct's tail padding are repurposed as the emitter world origin — positional
// force fields (Vortex/Orbit/Scatter) need it since particles are world-space
// but the field centres are emitter-local. Stays 64 B (3 scalar floats pack at
// offsets 52/56/60; std430 scalar align = 4).
struct alignas(16) Particle {
    float pos_x, pos_y, pos_z, age;
    float vel_x, vel_y, vel_z, life;
    u32   colorRGBA;
    float rot;
    float size_x, size_y;
    u32   defId;
    float origin_x, origin_y, origin_z;   // emitter world spawn position
};
static_assert(sizeof(Particle) == 64, "GPUParticle 64 B");

// Push constants for compute dispatches (must match gp_common.glsl GPPush).
struct ComputePush {
    float spawnPos[4];      // xyz = emitter pos, w = emitCount (as float-bits uint)
    float dt_gravity[4];    // [0] = dt, [1] = gravity magnitude
    float camPos[4];        // xyz = camera position (Phase 4 depth sort)
    float camForward[4];    // xyz = camera forward (Phase 4 depth sort)
    u32   maxParticles;
    u32   frameSeed;
    u32   activeProgram;    // reserved (emit now reads program per spawn request)
    u32   numRequests;      // active spawn requests this frame
    u32   numKills;         // emitter kill requests this frame
};
static_assert(sizeof(ComputePush) <= 128, "ComputePush <= 128 B");

// One emit request — must match gp_common.glsl SpawnRequest (48 B std430).
// CPU appends one per active emitter per frame; gp_emit walks them.
struct SpawnRequest {
    float pos[4];           // xyz = world spawn position; w = per-program alive cap (uint bits, 0 = uncapped)
    float vel[4];           // xyz = inherited parent velocity
    u32   program;          // program index (becomes particle.defId)
    u32   count;            // particles this request spawns this frame
    u32   firstSlot;        // base emit-gid this request owns
    u32   seed;             // RNG seed base
};
static_assert(sizeof(SpawnRequest) == 48, "SpawnRequest 48 B std430");

// Push constants for the graphics draw (must match gp_particle.vert.glsl PC).
// The tail 4 vec4s also carry the froxel volumetric light-probe params (Stage-0
// smoke lighting, matching the CPU particle pass) — packed into the unused .w
// lanes + two extra vectors to stay within the 128 B guaranteed push size.
struct DrawPush {
    float viewProj[16];     // combined view-projection (row-major Fmatrix)
    float camRight[4];      // xyz = camera right; w = froxel near Z
    float camUp[4];         // xyz = camera up;    w = log2(far/near)
    float camPosStr[4];     // xyz = camera world pos; w = probe strength (0 = off)
    float camDirClamp[4];   // xyz = camera forward;   w = radiance clamp
};
static_assert(sizeof(DrawPush) <= 128, "DrawPush <= 128 B");

// --- Phase 2: action-program (must match gp_common.glsl std430 layout) ----

// One pDomain resolved for GPU sampling (mirrors PAPI::pDomain essentials).
struct GenDomain {              // 96 B
    u32   dtype;               // PDomainEnum
    u32   _gp0, _gp1, _gp2;
    float p1[4];               // point / box-min / sphere-centre / line-start
    float p2[4];               // box-max / line-delta / cyl axis vector
    float u[4];                // 'u' basis (cyl/cone/disc/rect)
    float v[4];                // 'v' basis
    float radii[4];            // radius1, radius2, radius1Sqr, radius2Sqr
};
static_assert(sizeof(GenDomain) == 96, "GenDomain 96 B std430");

// One simulate-time action: atype = PActionEnum; a/b/c hold params.
struct ActionRec {             // 64 B
    u32   atype;
    u32   _ap0, _ap1, _ap2;
    float a[4], b[4], c[4];
};
static_assert(sizeof(ActionRec) == 64, "ActionRec 64 B std430");

// Source action resolved for the emit shader.
struct EmitDesc {              // 512 B
    GenDomain posDom, velDom, sizeDom, colorDom, rotDom;
    float sc[4];               // alpha, age, age_sigma, lifetime
    float parentVel[4];        // xyz
};
static_assert(sizeof(EmitDesc) == 512, "EmitDesc 512 B std430");

constexpr u32 kMaxActions = 32;
struct Program {               // 16 + 512 + 32*64 = 2576 B
    u32   actionCount;
    float emitRate;            // particles/sec
    u32   progFlags;
    u32   _prog_pad;
    EmitDesc  emit;
    ActionRec actions[kMaxActions];
};
static_assert(sizeof(Program) == 16 + 512 + kMaxActions * 64, "Program std430");

// Per-program texture/atlas/orientation metadata — must match gp_particle.vert
// TexInfo (48 B std430). Indexed by particle defId; the vertex shader computes
// the atlas frame UV, the billboard axes and the bindless texture slot.
struct TexInfo {
    s32   layer;               // bindless texture slot (= program index), or -1 (untextured)
    u32   flags;               // bit0 framed, bit1 animated, bit2 random-frame, bit3 align-to-path
    float frameSize[2];        // UV size of one atlas frame (1,1 if not framed)
    u32   frameDimX;           // atlas columns
    u32   frameCount;          // total frames
    float frameSpeed;          // frames/sec (dfAnimated)
    float _texpad;
    // dfAlignToPath axis for zero-velocity particles (from APDefaultRotation) —
    // campfire flames are UPRIGHT billboards (T = world-up) that only yaw to
    // the camera; a camera-facing quad lies flat when looked at from above.
    float alignDir[4];
};
static_assert(sizeof(TexInfo) == 48, "TexInfo 48 B std430");

// Texture/atlas description a translated effect carries back to the GP module,
// which loads the sprite texture into a bindless slot and fills a TexInfo.
struct TexDesc {
    char  name[256];           // sprite texture name (empty → untextured)
    u32   flags;               // bit0 framed, bit1 animated, bit2 random-frame, bit3 align-to-path
    u32   frameDimX;
    u32   frameCount;
    float frameW, frameH;      // UV size of one frame
    float frameSpeed;
    float alignDir[3];         // bit3: billboard T axis when velocity ~0
};

// One emitter's spawn intent for a frame, resolved by the CPU. The dispatcher
// converts these into SpawnRequests (assigning gid ranges). Used both for the
// camera/gp_spawn emitters and the pulled real-.pe world emitters (Phase 3 #3).
struct EmitterSample {
    u32     program;        // program registry slot
    u32     count;          // particles to spawn this frame
    Fvector pos;            // world spawn position
    Fvector vel;            // inherited parent velocity
    bool    hud = false;    // HUD emitter (muzzle flash) → HUD-FOV projection at draw
};

bool Init();
void Destroy();
void DispatchComputeAndDraw(FrameContext& ctx);
// RENDER_THREAD_PLAN step 7: pull the world .pe emitters off the live effects (PREPARE phase, extract side).
void ExtractWorldEmitters();

// --- #6 (drop CPU double-sim): GPU smoke → froxel media splat ---------------
// Vol::Execute records this between its accum clear and resolve so GPU-routed
// smoke keeps feeding the volumetric fog after the CPU sim stops. Layout is
// vol_splat's 112 B SplatPush with the spare .w lanes carrying the distance-LOD
// zone (camPos.w = full-quality radius, camDir.w = media cutoff); params[0] is
// overwritten by SplatMedia with the pool capacity.
struct MediaSplatPush {
    float camPos[4], camDir[4], camRightT[4], camTopT[4], zParams[4], dims[4], params[4];
};
static_assert(sizeof(MediaSplatPush) == 112, "MediaSplatPush 112 B");

// True when a media splat would record anything (module live + pool initialised).
bool WantsMediaSplat();

// Record the media splat dispatch into cmd: reads LAST frame's world-alpha
// alive region (this runs before the frame's gp_reset/sim) and atomic-adds
// into Vol's accum SSBO. Lazily builds its pipeline/set; rebinds when accumBuf
// changes. Returns false if nothing was recorded.
bool SplatMedia(VkCommandBuffer cmd, VkBuffer accumBuf, const MediaSplatPush& push);

// True when the module is live and r_gpu_particles is on (cheap; safe anytime).
bool Enabled();

// Resolve an effect name to a program-registry slot, translating+registering on
// first use. Returns the slot (>=0), 0 for empty name (campfire), or -1 on
// failure. Cached by name — repeat calls are a cheap lookup.
int ResolveProgram(const char* name);

// Particles/sec of a registered program slot (0 if out of range).
float ProgramRate(int slot);

// Particle lifetime / authored per-instance budget of a registered slot
// (0 if out of range). The feed uses them for per-instance spawn ownership.
float ProgramLife(int slot);
u32   ProgramMaxP(int slot);

// Whether an effect is light enough to auto-route onto the shared GPU pool
// (rate × life budget). Area fog / persistent fields fail this and stay on CPU.
bool ProgramRoutable(int slot);

// #5: live-instance registry per program — the alive cap must count ALL live
// effect objects (CPU parity: a per-instance PAPI pool existed while its object
// lived, visible or not), not just this frame's frustum-visible emitters, or
// one campfire's immortal flame starves every off-screen sibling. Called from
// vkCParticleEffect (claim resolve / destructor); thread-safe.
void AddProgramInstance(int slot);
void ReleaseProgramInstance(int slot);

// CPU parity for hard stops / object destruction: PAPI wipes the instance's
// particles instantly (p_count = 0); GPU particles are pool-resident, so the
// effect queues a kill and gp_simulate culls its particles next frame
// (matched by program + spawn-time emitter origin within radius).
void QueueKill(int slot, const Fvector& pos, float radius);

// Phase 3 — real .pe translator (vk_gpu_particles_translate.cpp). Walks a named
// effect's loaded PAPI action list into the GPU Program above. Returns false
// (caller keeps its current program) if missing/empty/has no Source.
// outMaxParticles = the def's authored per-instance budget (CPEDef::m_MaxParticles).
bool TranslateEffect(const char* pedName, Program& out, float& outEmitRate, TexDesc& outTex, u32& outMaxParticles);

// Console `gp_debug <effect>` — dump the def (flags/frame) + translated program
// to the log (GPU-vs-CPU mismatch diagnosis). vk_gpu_particles_translate.cpp.
void DebugEffect(const char* name);

// Console `gp_stats` — per-program budget/alive counters to the log.
void DumpStats();

// Re-point the camera-pinned emitter at a real effect by name (console
// `gp_mirror`). Empty/null name reverts to the authored campfire test program.
// Safe no-op if the module isn't initialised yet (set r_gpu_particles 1 first).
bool MirrorEffect(const char* name);

// Phase 3 step 2 — spawn a persistent WORLD emitter of the named effect at the
// current camera position (console `gp_spawn`). Unlike gp_mirror it does not
// follow the camera, so multiple emitters can coexist in the world.
bool SpawnEffect(const char* name);

// Remove all world emitters placed by SpawnEffect (console `gp_spawn_clear`).
// The camera-pinned emitter (gp_mirror) is left untouched.
void ClearSpawns();

}}  // namespace VK::GPUParticles
