#version 450
#extension GL_GOOGLE_include_directive : require

// Phase 2 simulate: one thread per pool slot. Each live particle runs the
// effect's ACTION PROGRAM in list order — a GPU port of the PAPI per-action
// Execute() bodies (Tier A). All Tier-A actions are per-particle and order-
// independent across particles, so this particle-major loop matches PAPI's
// action-major loop exactly.
//
// Dead particles (KillOld) push their slot back onto the free-list (atomic
// stack push); survivors append their index to aliveList for the indirect draw.

#include "gp_common.glsl"

layout(local_size_x = 64) in;

#define STEP_DEFAULT 0.033

void main()
{
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= pc.maxParticles) return;

    GPUParticle p = pool[idx];
    if (p.pos_age.w < 0.0) return;   // dead slot — skip

    float dt    = pc.dt_gravity.x;
    float life  = p.vel_life.w;      // tm_max for time-scaled actions

    vec3  pos   = p.pos_age.xyz;
    float age   = p.pos_age.w;
    vec3  vel   = p.vel_life.xyz;
    vec2  size  = p.size;
    float rot   = p.rot;
    vec3  origin = vec3(p.origin_x, p.origin_y, p.origin_z);  // emitter world pos
    vec4  col   = gp_unpackBGRA(p.colorRGBA);  // rgba 0..1
    bool  dead  = false;

    // Per-particle, per-frame RNG stream for the Random* actions. Varying by
    // slot + frame gives each particle an independent jitter every step, like
    // PAPI's drand48-driven domain sampling.
    uint rng = gp_hash(idx * 2654435761u + pc.frameSeed);

    uint pi  = p.defId & 0x7FFFFFFFu;     // per-particle program (registry index)
    bool hud = (p.defId & 0x80000000u) != 0u;   // bit31 = HUD emitter (muzzle flash)
    uint n  = programs[pi].actionCount;
    for (uint i = 0u; i < n; ++i) {
        ActionRec act = programs[pi].actions[i];
        switch (act.atype) {

        case GP_GRAVITY:        // vel += direction * dt
            vel += act.a.xyz * dt;
            break;

        case GP_MOVE:           // age += dt; pos += vel * dt
            age += dt;
            pos += vel * dt;
            break;

        case GP_DAMPING: {      // vel *= 1 - (1-damping)*dt, if speed in band
            float vSqr = dot(vel, vel);
            if (vSqr >= act.b.x && vSqr <= act.b.y) {
                vec3 scale = vec3(1.0) - (vec3(1.0) - act.a.xyz) * dt;
                vel *= scale;
            }
            break;
        }

        case GP_SPEEDLIMIT: {   // clamp |vel| into [min,max]
            float minS = act.a.x, maxS = act.a.y;
            float sSqr = dot(vel, vel);
            if (sSqr < minS * minS && sSqr > 0.0)      vel *= (minS / sqrt(sSqr));
            else if (sSqr > maxS * maxS)               vel *= (maxS / sqrt(sSqr));
            break;
        }

        case GP_TARGETVELOCITY: // vel += (target - vel) * scale*dt
            vel += (act.a.xyz - vel) * (act.b.x * dt);
            break;

        case GP_TARGETSIZE: {   // size += (target - size) * scale*dt  (per axis)
            vec2 dif = (act.a.xy - size) * (act.b.xy * dt);
            size += dif;
            break;
        }

        case GP_TARGETCOLOR: {  // ease colour/alpha toward target within [from,to]*life
            float tmFrom = act.b.y * life;
            float tmTo   = act.b.z * life;
            if (age >= tmFrom && age <= tmTo) {
                float COEFF    = STEP_DEFAULT / max(dt, 1e-6);
                float scaleFac = act.b.x * STEP_DEFAULT;
                vec4  target   = vec4(act.a.xyz, act.a.w);
                vec4  cn       = mix(col, target, scaleFac);
                col -= (col - cn) / COEFF;
            }
            break;
        }

        case GP_KILLOLD: {      // kill if !((age < limit) ^ kill_less_than)
            bool less = act.a.y != 0.0;
            bool young = age < act.a.x;
            if (!(young != less)) dead = true;   // (young ^ less) == false → kill
            break;
        }

        // ---- Tier-B/C ------------------------------------------------------

        case GP_RANDOMVELOCITY: // vel = sample(domain)   (reassigned each step)
            vel = gp_genCompact(act._ap0, act.a.xyz, act.b.xyz, act.c.xy, rng);
            break;

        case GP_RANDOMACCEL:    // vel += sample(domain) * dt
            vel += gp_genCompact(act._ap0, act.a.xyz, act.b.xyz, act.c.xy, rng) * dt;
            break;

        case GP_RANDOMDISPLACE: // pos += sample(domain) * dt
            pos += gp_genCompact(act._ap0, act.a.xyz, act.b.xyz, act.c.xy, rng) * dt;
            break;

        // Positional force fields. Centres/points are emitter-LOCAL in the
        // program; rebuild the world-space centre = emitter origin + local
        // (assumes emitter has no rotation, same approximation as directionL).
        case GP_ORBITPOINT: {   // accelerate toward a point (1/r^2 softened)
            vec3  center = origin + act.a.xyz;
            float mag = act.c.x, eps = act.c.y, maxrad = act.c.z;
            vec3  dir  = center - pos;
            float rSqr = dot(dir, dir);
            if (rSqr <= maxrad * maxrad)
                vel += dir * ((mag * dt) / (sqrt(rSqr) + (rSqr + eps)));
            break;
        }

        case GP_ORBITLINE: {    // accelerate toward a line
            vec3  pL   = origin + act.a.xyz;
            vec3  axis = act.b.xyz;             // author-normalised direction
            float mag = act.c.x, eps = act.c.y, maxrad = act.c.z;
            vec3  f    = pos - pL;
            vec3  into = axis * dot(f, axis) - f;
            float rSqr = dot(into, into);
            if (rSqr <= maxrad * maxrad)
                vel += into * ((mag * dt) / (sqrt(rSqr) + (rSqr + eps)));
            break;
        }

        case GP_SCATTER: {      // accelerate away from a centre
            vec3  center = origin + act.a.xyz;
            float mag = act.c.x, eps = act.c.y, maxrad = act.c.z;
            vec3  dir  = pos - center;
            float rSqr = dot(dir, dir);
            if (rSqr <= maxrad * maxrad) {
                vec3 accel = dir / sqrt(max(rSqr, 1e-12));
                vel += accel * ((mag * dt) / (rSqr + eps));
            }
            break;
        }

        case GP_VORTEX: {       // rotate position around an axis about a centre
            vec3  center = origin + act.a.xyz;
            vec3  axis   = normalize(act.b.xyz);
            float mag = act.c.x, eps = act.c.y, maxrad = act.c.z;
            vec3  off  = pos - center;
            float rSqr = dot(off, off);
            if (rSqr <= maxrad * maxrad) {
                float r     = sqrt(max(rSqr, 1e-12));
                vec3  offn  = off / r;
                vec3  w     = axis * dot(offn, axis);   // parallel component
                vec3  u     = offn - w;                 // perpendicular
                vec3  vv    = cross(axis, u);
                float theta = (mag * dt) / (rSqr + eps);
                pos = (u * cos(theta) + vv * sin(theta) + w) * r + center;
            }
            break;
        }

        case GP_TURBULENCE: {   // perlin-gradient velocity perturbation (speed-preserving)
            vec3  offset  = act.a.xyz;
            float freq    = act.a.w;
            int   octaves = clamp(int(act.b.x), 1, 6);
            float mag = act.b.y, eps = act.b.z;
            vec3  pV = pos + offset * pc.dt_gravity.z;   // z = global scroll time
            float d  = gp_fractalsum3(pV, freq, octaves);
            vec3  D  = vec3(gp_fractalsum3(pV + vec3(eps, 0, 0), freq, octaves),
                            gp_fractalsum3(pV + vec3(0, eps, 0), freq, octaves),
                            gp_fractalsum3(pV + vec3(0, 0, eps), freq, octaves));
            float vmOld = length(vel);
            vel += (D - vec3(d)) * mag;
            float vmNew = length(vel);
            if (vmNew > 1e-8) vel *= (vmOld / vmNew);    // keep original speed
            break;
        }

        case GP_TARGETROTATE: { // ease rotation angle toward a target
            float r        = abs(act.a.x);
            float scaleFac = act.b.x * dt;
            float sgn      = rot >= 0.0 ? scaleFac : -scaleFac;
            rot += (r - abs(rot)) * sgn;
            break;
        }

        default:
            break;              // unsupported action → no-op (CPU-fallback later)
        }
    }

    // Emitter kills (hard Stop / object destruction — CPU wipes instantly).
    // Matched by program + SPAWN-time emitter origin; radius covers drift.
    // Age floor: a kill queued last frame must not wipe a sibling particle
    // respawned THIS frame (stop→replay races on zone state changes).
    for (uint ki = 0u; ki < pc.numKills && !dead; ++ki) {
        if ((p.defId & 0x7FFFFFFFu) != killReqs[ki].program) continue;
        if (age < 0.15) continue;
        vec3 kd = origin - killReqs[ki].posRadius.xyz;
        if (dot(kd, kd) <= killReqs[ki].posRadius.w) dead = true;
    }

    if (dead) {
        p.pos_age.w = -1.0;
        pool[idx]   = p;
        uint fi = atomicAdd(counters[0], 1u);    // push slot back to free-list
        freeList[fi] = idx;
        // #5: release the per-program alive budget (defId bit31 = HUD → mask).
        // Underflow guard mirrors the free-list pop: particles emitted while
        // their program was uncapped would otherwise wrap the counter.
        uint bp = p.defId & 0x7FFFFFFFu;
        uint aprev = atomicAdd(progAlive[bp], uint(-1));
        if (aprev == 0u || aprev > pc.maxParticles) atomicAdd(progAlive[bp], 1u);
        return;
    }

    p.pos_age   = vec4(pos, age);
    p.vel_life  = vec4(vel, life);
    p.size      = size;
    p.rot       = rot;
    p.colorRGBA = gp_packBGRA(col);
    pool[idx]   = p;

    // Route into one of 4 alive groups so the draw can issue one indirect draw
    // per (projection × blend) combo: world/HUD × alpha/additive. aliveList has
    // two maxP regions — world [0,maxP), HUD [maxP,2maxP). Within each, alpha
    // grows from the FRONT, additive from the BACK (can't collide: per-region
    // total <= maxP). Counters: [1]=world-alpha [2]=world-add [3]=hud-alpha [4]=hud-add.
    uint maxP = pc.maxParticles;
    uint base = hud ? maxP : 0u;                 // region base offset
    bool additive = (programs[pi].progFlags & 1u) != 0u;
    if (additive) {
        uint ai = atomicAdd(counters[hud ? 4u : 2u], 1u);
        aliveList[base + maxP - 1u - ai] = idx;  // from the back of the region
    } else {
        uint ai = atomicAdd(counters[hud ? 3u : 1u], 1u);
        aliveList[base + ai] = idx;              // from the front of the region
    }
}
