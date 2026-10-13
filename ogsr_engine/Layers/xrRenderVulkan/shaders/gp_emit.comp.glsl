#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 3 step 2 emit: a single dispatch services many emitters. Each spawn
// request owns a contiguous gid range [firstSlot, firstSlot+count); this
// invocation finds its request, pops a free slot (atomic stack pop) and spawns
// one particle from that request's program + world position. Domains are
// sampled exactly like PAPI::PASource. gp_emitCount() = total particles to try.

#include "gp_common.glsl"

layout(local_size_x = 64) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= gp_emitCount()) return;

    // ---- Which emitter owns this invocation? (numRequests is small) -------
    uint r = 0xFFFFFFFFu;
    for (uint i = 0u; i < pc.numRequests; ++i) {
        uint fs = spawnReqs[i].firstSlot;
        if (gid >= fs && gid < fs + spawnReqs[i].count) { r = i; break; }
    }
    if (r == 0xFFFFFFFFu) return;       // gap (shouldn't happen — ranges are dense)
    SpawnRequest sr = spawnReqs[r];

    // program low 31 bits = registry index; bit31 = HUD emitter (muzzle flash).
    uint pi = sr.program & 0x7FFFFFFFu;

    // ---- #5 per-program alive budget (claim BEFORE the free-list pop) -----
    // cap = m_MaxParticles × live instances (CPU-computed, in sr.pos.w).
    // At the cap the birth is dropped — exactly what PAPI does at max_particles.
    uint cap = floatBitsToUint(sr.pos.w);
    if (cap != 0u) {
        uint aprev = atomicAdd(progAlive[pi], 1u);
        if (aprev >= cap) { atomicAdd(progAlive[pi], uint(-1)); return; }
    }

    // ---- Free-list pop (stack) --------------------------------------------
    // prev = old freeCount; decrement (unsigned wrap). If it was empty (0) or
    // a concurrent underflow wrapped it past maxParticles, roll back and bail
    // (returning the budget claim too).
    uint prev = atomicAdd(counters[0], uint(-1));
    if (prev == 0u || prev > pc.maxParticles) {
        atomicAdd(counters[0], 1u);
        if (cap != 0u) atomicAdd(progAlive[pi], uint(-1));
        return;
    }
    uint slot = freeList[prev - 1u];

    // ---- Sample the Source domains (from this request's program) ----------
    EmitDesc e = programs[pi].emit;
    uint local = gid - sr.firstSlot;
    uint seed  = sr.seed + local * 9277u + 0x9E3779B9u;

    vec3 lpos = gp_generate(e.posDom,   seed);   // local-space spawn offset
    vec3 vel  = gp_generate(e.velDom,   seed) + e.parentVel.xyz + sr.vel.xyz;
    vec3 siz  = gp_generate(e.sizeDom,  seed);
    vec3 col  = gp_generate(e.colorDom, seed);
    vec3 rt   = gp_generate(e.rotDom,   seed);

    float life = e.sc.w;
    float age  = e.sc.y;        // age_sigma (sc.z) ignored for now

    GPUParticle p;
    p.pos_age   = vec4(sr.pos.xyz + lpos, age);
    p.vel_life  = vec4(vel, life);
    p.colorRGBA = gp_packBGRA(vec4(col, e.sc.x));
    p.rot       = rt.x;
    p.size      = siz.xy;
    p.defId     = sr.program;   // low 31 bits = program; bit31 = HUD (draw routing)
    p.origin_x  = sr.pos.x;     // emitter world origin (for positional force fields)
    p.origin_y  = sr.pos.y;
    p.origin_z  = sr.pos.z;

    pool[slot] = p;
}
