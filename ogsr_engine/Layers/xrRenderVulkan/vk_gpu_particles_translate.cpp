// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// GPU-Driven Particles — Phase 3: real .pe -> GPU action-program translator.
//
// Walks a CPEDef's loaded PAPI action list and serialises it into the same
// VK::GPUParticles::Program struct the GPU interpreter already runs (Phase 2a).
// The struct layouts were designed for exactly this — domains copy verbatim
// (pDomain field storage matches gp_generate) and our GP_* action ids equal
// PActionEnum, so an action's `type` is its GPU opcode directly.
//
// Kept in a separate TU so the heavy ParticleEffectDef.h / Shader.h include
// chain stays out of the clean GPU module (vk_gpu_particles.cpp). The output
// Program is a POD declared in vk_gpu_particles.h with no PAPI dependency.
#include "stdafx.h"

// ParticleEffectDef.h -> Shader.h -> tss_def.h needs the xrD3DDefs Vulkan-branch
// D3D type stubs; the skeleton compat preamble establishes them (same setup as
// vk_ParticleEffect.cpp / vk_ParticleEffectDef.cpp).
#define FBasicVisualH
#include "vk_FBasicVisual.h"
#include "vk_color_space.h"   // ColorSpace — particle colours are authored sRGB
#include "CRender_Vulkan.h"
#include "vk_ModelPool.h"

#include "../../xrParticles/psystem.h"
#include "../../xrParticles/particle_manager.h"
#include "../../xrParticles/particle_actions_collection.h"
#include "../xrRender/ParticleEffectDef.h"

#include "vk_gpu_particles.h"

using namespace PAPI;

// PS library lookup (vk_PSLibrary.cpp), same extern vk_ParticleEffect.cpp uses.
extern PS::CPEDef* VK_FindPED(const char* name);

// GPU particle lifetime clamp (vk_console_min.cpp) — see the cap block below.
extern float ps_r_gpu_particles_life_cap;

namespace VK { namespace GPUParticles {

namespace {
    // pDomain field storage matches gp_generate's expectations byte-for-byte
    // (see particle_core.cpp pDomain ctor/Generate vs gp_common.glsl), so a
    // plain field copy is a faithful translation for every domain type.
    void FillGenDomain(GenDomain& g, const pDomain& d)
    {
        g = {};
        g.dtype = (u32)d.type;
        g.p1[0] = d.p1.x; g.p1[1] = d.p1.y; g.p1[2] = d.p1.z;
        g.p2[0] = d.p2.x; g.p2[1] = d.p2.y; g.p2[2] = d.p2.z;
        g.u[0]  = d.u.x;  g.u[1]  = d.u.y;  g.u[2]  = d.u.z;
        g.v[0]  = d.v.x;  g.v[1]  = d.v.y;  g.v[2]  = d.v.z;
        g.radii[0] = d.radius1;    g.radii[1] = d.radius2;
        g.radii[2] = d.radius1Sqr; g.radii[3] = d.radius2Sqr;
    }

    // Decode an emit COLOUR domain from sRGB to linear (r_linear_color). Colour is the
    // one domain whose numbers are a colour rather than a position/size, so it is the
    // only one that gets converted — hence a dedicated function instead of a flag on
    // FillGenDomain, which would invite passing it for posDom by accident.
    //
    // Only the domain shapes whose p1/p2 are literally COLOURS are handled: PDPoint
    // (p1 = the colour) and PDBox (p1/p2 = corner colours). Others encode p2 as a delta
    // or an axis (PDLine) or carry basis vectors, where a per-component decode would be
    // meaningless — those are left in gamma and logged once, the same "be honest about
    // the mismatch" policy the unsupported-action path below uses. In practice stock and
    // modded effects author colour as a point or a box.
    void LinearizeColorDomain(GenDomain& g)
    {
        if (!VK::ColorSpace::Active()) return;
        if (g.dtype == (u32)PDPoint) {
            VK::ColorSpace::LinearizeRGB(g.p1);
        } else if (g.dtype == (u32)PDBox) {
            VK::ColorSpace::LinearizeRGB(g.p1);
            VK::ColorSpace::LinearizeRGB(g.p2);
        } else {
            static bool s_warned = false;
            if (!s_warned) { s_warned = true;
                Msg("![VK GP] colour domain type %u left in gamma space (only PDPoint/PDBox are decoded) "
                    "— GPU particles of this effect will not match the CPU path under r_linear_color", g.dtype);
            }
        }
    }

    // Actions the gp_simulate interpreter understands. Tier-A (Phase 2a) plus
    // the frame-invariant Tier-B/C set (Random*/Turbulence/TargetRotate) added
    // in the full-interpreter pass. An action outside this set is serialised as
    // an UNSUPPORTED sentinel (preserving list order/count) and no-ops in the
    // shader — counted + logged here so the mismatch with CPU is honest.
    bool IsInterpreted(u32 t)
    {
        switch (t) {
        // Tier-A
        case PADampingID: case PAGravityID: case PAKillOldID: case PAMoveID:
        case PASpeedLimitID: case PATargetColorID: case PATargetSizeID:
        case PATargetVelocityID: case PATargetVelocityDID:
        // Tier-B/C (frame-invariant — correct in world space without an
        // emitter origin; positional force fields Vortex/Orbit/Scatter wait
        // for the per-particle origin pass).
        case PARandomVelocityID: case PARandomAccelID: case PARandomDisplaceID:
        case PATurbulenceID: case PATargetRotateID: case PATargetRotateDID:
        // Positional force fields (world centre = emitter origin + local centre).
        case PAVortexID: case PAOrbitPointID: case PAOrbitLineID: case PAScatterID:
            return true;
        default:
            return false;
        }
    }

    // gp_simulate ignores any action carrying this opcode (its `default` case).
    // Used for actions we don't interpret yet, so an unsupported action can
    // never accidentally drive a live opcode's case with zero params.
    constexpr u32 kUnsupportedOp = 0xFFFFFFFFu;

    // Blend class for the draw: 1 = additive pipeline (fire/sparks/muzzle),
    // 0 = alpha pipeline (smoke). Mirrors vkCParticleEffect::DetermineBlendMode's
    // shader-name precedence so the GPU routes the SAME way the CPU classifies
    // (PBM_ADD / PBM_ALPHA_ADD → additive; PBM_BLEND → alpha; default → additive).
    u32 BlendClassFromShader(const char* shaderName)
    {
        if (!shaderName || !shaderName[0]) return 1u;   // DetermineBlendMode default = PBM_ADD
        string256 s; xr_strcpy(s, sizeof(s), shaderName); _strlwr(s);
        if (strstr(s, "distort"))                                              return 0u; // not routed
        if (strstr(s, "alpha_add") || strstr(s, "alpha-add") || strstr(s, "aadd")) return 1u;
        if (strstr(s, "add"))                                                  return 1u;
        if (strstr(s, "mul"))                                                  return 0u; // not routed
        if (strstr(s, "set"))                                                  return 0u; // not routed
        if (strstr(s, "blend"))                                                return 0u;
        return 1u;                                                                        // default PBM_ADD
    }

    // Pack a reduced pDomain (dtype + p1 + p2 + radii, no u/v basis) into an
    // ActionRec for the simulate-time Random* samplers (gp_genCompact). Covers
    // Point/Line/Box/Sphere/Blob — the domains these actions use in content.
    void FillCompactDomain(ActionRec& a, const pDomain& d)
    {
        a._ap0 = (u32)d.type;
        a.a[0] = d.p1.x; a.a[1] = d.p1.y; a.a[2] = d.p1.z;
        a.b[0] = d.p2.x; a.b[1] = d.p2.y; a.b[2] = d.p2.z;
        a.c[0] = d.radius1; a.c[1] = d.radius2;
    }
}

// Translate a named .pe into the GPU Program. Returns false (caller keeps its
// current program) if the effect is missing, empty, or has no Source action.
bool TranslateEffect(const char* pedName, Program& out, float& outEmitRate, TexDesc& outTex, u32& outMaxParticles)
{
    outTex = {};
    outMaxParticles = 0;
    PS::CPEDef* def = VK_FindPED(pedName);
    if (!def) { Msg("![VK GP] gp_mirror: effect '%s' not found", pedName); return false; }
    if (!def->m_Actions.size()) { Msg("![VK GP] gp_mirror: '%s' has no actions", pedName); return false; }

    // #5: the authored per-INSTANCE particle budget — the CPU path enforces it
    // via SetMaxParticles; the GPU pool enforces cap = this × live instances.
    outMaxParticles = def->m_MaxParticles;

    // World-aligned billboards (speed<eps branch with a FIXED world frame) are
    // not representable by the GPU draw's axis model yet — keep them on CPU.
    if (def->m_Flags.is(PS::CPEDef::dfAlignToPath) && def->m_Flags.is(PS::CPEDef::dfWorldAlign)) {
        Msg("~[VK GP] '%s' uses AlignToPath+WorldAlign — kept on CPU", pedName);
        return false;
    }

    // Sprite texture + atlas frame metadata (mirrors the CPU billboard path:
    // dfFramed → CalculateTC sub-rects; dfAnimated advances frames over life).
    if (def->m_TextureName.size())
        xr_strcpy(outTex.name, sizeof(outTex.name), def->m_TextureName.c_str());
    if (def->m_Flags.is(PS::CPEDef::dfFramed))      outTex.flags |= 1u;
    if (def->m_Flags.is(PS::CPEDef::dfAnimated))    outTex.flags |= 2u;
    if (def->m_Flags.is(PS::CPEDef::dfRandomFrame)) outTex.flags |= 4u;
    // dfAlignToPath: billboard T axis = velocity dir, or — for zero-velocity
    // particles (campfire flames!) — the authored default rotation. CPU:
    // dir.setHP(-rot.y, -rot.x) in BuildVertices; flames get (0,1,0) = upright.
    if (def->m_Flags.is(PS::CPEDef::dfAlignToPath)) {
        outTex.flags |= 8u;
        Fvector d;
        d.setHP(-def->m_APDefaultRotation.y, -def->m_APDefaultRotation.x);
        outTex.alignDir[0] = d.x; outTex.alignDir[1] = d.y; outTex.alignDir[2] = d.z;
    }
    outTex.frameDimX   = (u32)_max(def->m_Frame.m_iFrameDimX, 1);
    outTex.frameCount  = (u32)_max(def->m_Frame.m_iFrameCount, 1);
    outTex.frameW      = def->m_Frame.m_fTexSize.x;
    outTex.frameH      = def->m_Frame.m_fTexSize.y;
    outTex.frameSpeed  = def->m_Frame.m_fSpeed;

    auto* mgr = static_cast<CParticleManager*>(ParticleManager());
    const int al = mgr->CreateActionList();
    if (al < 0) { Msg("![VK GP] gp_mirror: CreateActionList failed"); return false; }

    {
        IReader F(def->m_Actions.pointer(), def->m_Actions.size());
        mgr->LoadActions(al, F, def->m_copFormat);
    }
    ParticleActions* list = mgr->GetActionListPtr(al);
    if (!list || list->empty()) {
        mgr->DestroyActionList(al);
        Msg("![VK GP] gp_mirror: '%s' produced no runtime actions", pedName);
        return false;
    }

    out = {};

    // Draw blend class → progFlags bit0 (gp_simulate routes additive particles
    // to the additive draw pipeline; alpha ones to the alpha pipeline).
    out.progFlags = BlendClassFromShader(def->m_ShaderName.size() ? def->m_ShaderName.c_str() : nullptr);

    // Particle lifetime (sc.w / vel_life.w, used by time-scaled actions) comes
    // from KillOld's age_limit — resolve it before the Source fills its scalars.
    float lifetime = 100.0f;
    bool  haveKill = false, haveMove = false;
    for (PAVecIt it = list->begin(); it != list->end(); ++it) {
        if ((*it)->type == PAKillOldID) {
            PAKillOld* k = static_cast<PAKillOld*>(*it);
            if (!k->kill_less_than) { lifetime = k->age_limit; haveKill = true; }
        }
    }

    bool haveSource = false;
    u32  n = 0, skipped = 0, overflow = 0;

    auto push = [&](const ActionRec& a) {
        if (n < kMaxActions) out.actions[n++] = a;
        else ++overflow;
    };

    for (PAVecIt it = list->begin(); it != list->end(); ++it) {
        ParticleAction* pa = *it;
        switch (pa->type) {

        case PASourceID: {
            PASource* s = static_cast<PASource*>(pa);
            EmitDesc& e = out.emit;
            // Local-space domains: emit samples around the origin, the dispatch
            // offsets by the spawn position (camera, for this mirror path).
            FillGenDomain(e.posDom,   s->positionL);
            FillGenDomain(e.velDom,   s->velocityL);
            FillGenDomain(e.sizeDom,  s->size);
            FillGenDomain(e.colorDom, s->color);
            LinearizeColorDomain(e.colorDom);   // authored sRGB, like the CPU sprite path
            FillGenDomain(e.rotDom,   s->rot);
            e.sc[0] = s->alpha;
            e.sc[1] = s->age;
            e.sc[2] = s->age_sigma;
            e.sc[3] = lifetime;
            e.parentVel[0] = s->parent_vel.x;
            e.parentVel[1] = s->parent_vel.y;
            e.parentVel[2] = s->parent_vel.z;
            outEmitRate  = (s->particle_rate > 0.f) ? s->particle_rate : 1.0f;
            out.emitRate = outEmitRate;
            haveSource = true;
            break;
        }

        case PAGravityID: {
            PAGravity* g = static_cast<PAGravity*>(pa);
            ActionRec a{}; a.atype = PAGravityID;
            a.a[0] = g->directionL.x; a.a[1] = g->directionL.y; a.a[2] = g->directionL.z;
            push(a);
            break;
        }

        case PAMoveID: {
            ActionRec a{}; a.atype = PAMoveID; push(a);
            haveMove = true;
            break;
        }

        case PADampingID: {
            PADamping* d = static_cast<PADamping*>(pa);
            ActionRec a{}; a.atype = PADampingID;
            a.a[0] = d->damping.x; a.a[1] = d->damping.y; a.a[2] = d->damping.z;
            a.b[0] = d->vlowSqr;   a.b[1] = d->vhighSqr;
            push(a);
            break;
        }

        case PASpeedLimitID: {
            PASpeedLimit* sl = static_cast<PASpeedLimit*>(pa);
            ActionRec a{}; a.atype = PASpeedLimitID;
            a.a[0] = sl->min_speed; a.a[1] = sl->max_speed;
            push(a);
            break;
        }

        case PATargetColorID: {
            PATargetColor* t = static_cast<PATargetColor*>(pa);
            ActionRec a{}; a.atype = PATargetColorID;
            // The colour a particle eases toward over its life — authored sRGB, and the
            // easing happens in the shader against already-decoded emit colours, so the
            // target has to be decoded too or particles would drift toward a brighter
            // colour than the artist picked. a[3] is alpha (coverage) — untouched.
            a.a[0] = t->color.x; a.a[1] = t->color.y; a.a[2] = t->color.z; a.a[3] = t->alpha;
            VK::ColorSpace::LinearizeRGB(a.a);
            a.b[0] = t->scale;   a.b[1] = t->timeFrom; a.b[2] = t->timeTo;
            push(a);
            break;
        }

        case PATargetSizeID: {
            PATargetSize* t = static_cast<PATargetSize*>(pa);
            ActionRec a{}; a.atype = PATargetSizeID;
            a.a[0] = t->size.x;  a.a[1] = t->size.y;     // GPU size is 2D
            a.b[0] = t->scale.x; a.b[1] = t->scale.y;
            push(a);
            break;
        }

        case PATargetVelocityID: {
            PATargetVelocity* t = static_cast<PATargetVelocity*>(pa);
            ActionRec a{}; a.atype = PATargetVelocityID;
            a.a[0] = t->velocityL.x; a.a[1] = t->velocityL.y; a.a[2] = t->velocityL.z;
            a.b[0] = t->scale;
            push(a);
            break;
        }

        case PAKillOldID: {
            PAKillOld* k = static_cast<PAKillOld*>(pa);
            ActionRec a{}; a.atype = PAKillOldID;
            a.a[0] = k->age_limit; a.a[1] = k->kill_less_than ? 1.0f : 0.0f;
            push(a);
            break;
        }

        // ---- Tier-B/C: frame-invariant actions ---------------------------
        case PARandomVelocityID: {   // vel = sample(domain) each step
            PARandomVelocity* r = static_cast<PARandomVelocity*>(pa);
            ActionRec a{}; a.atype = PARandomVelocityID;
            FillCompactDomain(a, r->gen_velL);
            push(a);
            break;
        }

        case PARandomAccelID: {      // vel += sample(domain) * dt
            PARandomAccel* r = static_cast<PARandomAccel*>(pa);
            ActionRec a{}; a.atype = PARandomAccelID;
            FillCompactDomain(a, r->gen_accL);
            push(a);
            break;
        }

        case PARandomDisplaceID: {   // pos += sample(domain) * dt
            PARandomDisplace* r = static_cast<PARandomDisplace*>(pa);
            ActionRec a{}; a.atype = PARandomDisplaceID;
            FillCompactDomain(a, r->gen_dispL);
            push(a);
            break;
        }

        case PATurbulenceID: {       // perlin-gradient velocity perturbation
            PATurbulence* t = static_cast<PATurbulence*>(pa);
            ActionRec a{}; a.atype = PATurbulenceID;
            a.a[0] = t->offset.x; a.a[1] = t->offset.y; a.a[2] = t->offset.z; a.a[3] = t->frequency;
            a.b[0] = (float)t->octaves; a.b[1] = t->magnitude; a.b[2] = t->epsilon;
            push(a);
            break;
        }

        case PATargetRotateID:       // ease rotation angle toward target
        case PATargetRotateDID: {    // D variant → same opcode
            PATargetRotate* t = static_cast<PATargetRotate*>(pa);
            ActionRec a{}; a.atype = PATargetRotateID;
            a.a[0] = t->rot.x; a.b[0] = t->scale;
            push(a);
            break;
        }

        case PATargetVelocityDID: {  // same class as PATargetVelocity, alias opcode 27
            PATargetVelocity* t = static_cast<PATargetVelocity*>(pa);
            ActionRec a{}; a.atype = PATargetVelocityID;
            a.a[0] = t->velocityL.x; a.a[1] = t->velocityL.y; a.a[2] = t->velocityL.z;
            a.b[0] = t->scale;
            push(a);
            break;
        }

        // ---- Tier-B/C: positional force fields (centres are emitter-local;
        //      the shader rebuilds world centre = particle emitter origin + local) --
        case PAVortexID: {
            PAVortex* v = static_cast<PAVortex*>(pa);
            ActionRec a{}; a.atype = PAVortexID;
            a.a[0] = v->centerL.x; a.a[1] = v->centerL.y; a.a[2] = v->centerL.z;
            a.b[0] = v->axisL.x;   a.b[1] = v->axisL.y;   a.b[2] = v->axisL.z;
            a.c[0] = v->magnitude; a.c[1] = v->epsilon;   a.c[2] = v->max_radius;
            push(a);
            break;
        }

        case PAOrbitPointID: {
            PAOrbitPoint* o = static_cast<PAOrbitPoint*>(pa);
            ActionRec a{}; a.atype = PAOrbitPointID;
            a.a[0] = o->centerL.x; a.a[1] = o->centerL.y; a.a[2] = o->centerL.z;
            a.c[0] = o->magnitude; a.c[1] = o->epsilon;   a.c[2] = o->max_radius;
            push(a);
            break;
        }

        case PAOrbitLineID: {
            PAOrbitLine* o = static_cast<PAOrbitLine*>(pa);
            ActionRec a{}; a.atype = PAOrbitLineID;
            a.a[0] = o->pL.x;      a.a[1] = o->pL.y;      a.a[2] = o->pL.z;
            a.b[0] = o->axisL.x;   a.b[1] = o->axisL.y;   a.b[2] = o->axisL.z;
            a.c[0] = o->magnitude; a.c[1] = o->epsilon;   a.c[2] = o->max_radius;
            push(a);
            break;
        }

        case PAScatterID: {
            PAScatter* sc = static_cast<PAScatter*>(pa);
            ActionRec a{}; a.atype = PAScatterID;
            a.a[0] = sc->centerL.x; a.a[1] = sc->centerL.y; a.a[2] = sc->centerL.z;
            a.c[0] = sc->magnitude; a.c[1] = sc->epsilon;   a.c[2] = sc->max_radius;
            push(a);
            break;
        }

        default:
            // Unsupported here: emit the ignore-sentinel (keeps list order/count
            // so time-scaled indices still line up, but never triggers a live
            // opcode case with zero params). Positional force fields (Vortex/
            // Orbit/Scatter/OrbitLine) land here until per-particle emitter
            // origin is added — the shader has their cases ready.
            { ActionRec a{}; a.atype = kUnsupportedOp; push(a); }
            ++skipped;
            break;
        }
    }

    out.actionCount = n;
    mgr->DestroyActionList(al);

    if (!haveSource) {
        Msg("![VK GP] gp_mirror: '%s' has no Source action — cannot emit", pedName);
        return false;
    }
    if (overflow)
        Msg("~[VK GP] gp_mirror: '%s' has >%u actions — %u dropped", pedName, kMaxActions, overflow);
    if (!haveMove)
        Msg("~[VK GP] gp_mirror: '%s' has no Move action — particles won't integrate", pedName);
    if (!haveKill)
        Msg("~[VK GP] gp_mirror: '%s' has no KillOld — assuming %.0fs lifetime", pedName, lifetime);

    // Lifetime cap (r_gpu_particles_life_cap): "immortal" particles — campfire
    // flames are ONE billboard with KillOld 1000s — hold their #5 alive budget
    // forever, and since the cap counts only THIS frame's visible instances, an
    // off-screen ghost permanently starves every other campfire of its flame.
    // The CPU path never had this: its per-instance pools died with the object.
    // Clamp the spawned lifetime AND the KillOld limit (death goes through the
    // action, not vel_life.w); at flame emit rates the respawn lands the very
    // next frame — a 1-frame gap every cap seconds — and ghosts self-heal.
    const float lifeCap = ps_r_gpu_particles_life_cap;
    if (lifeCap > 0.f && lifetime > lifeCap) {
        out.emit.sc[3] = lifeCap;
        for (u32 i = 0; i < n; ++i)
            if (out.actions[i].atype == PAKillOldID && out.actions[i].a[1] == 0.0f && out.actions[i].a[0] > lifeCap)
                out.actions[i].a[0] = lifeCap;
        Msg("~[VK GP] '%s' life %.0fs clamped to %.0fs (alive-budget hygiene)", pedName, lifetime, lifeCap);
        lifetime = lifeCap;
    }

    Msg("[VK GPUParticles] mirrored '%s': %u actions (%u not interpreted), emitRate %.0f/s, life %.1fs",
        pedName, n, skipped, out.emitRate, lifetime);
    return true;
}

// ==========================================================================
// gp_debug <effect> — dump the def (flags/frame/tex) + the translated GPU
// program (domains, actions) so GPU-vs-CPU visual mismatches can be diagnosed
// straight from the log.
// ==========================================================================
void DebugEffect(const char* name)
{
    PS::CPEDef* def = VK_FindPED(name);
    if (!def) { Msg("![VK GP] gp_debug: effect '%s' not found (see gp_list)", name); return; }

    Msg("[VK GP] ===== gp_debug '%s' =====", name);
    Msg("  shader '%s' tex '%s' maxParticles %u",
        def->m_ShaderName.size() ? def->m_ShaderName.c_str() : "<none>",
        def->m_TextureName.size() ? def->m_TextureName.c_str() : "<none>",
        def->m_MaxParticles);
    Msg("  flags:%s%s%s%s%s%s%s%s%s%s%s",
        def->m_Flags.is(PS::CPEDef::dfFramed)         ? " Framed" : "",
        def->m_Flags.is(PS::CPEDef::dfAnimated)       ? " Animated" : "",
        def->m_Flags.is(PS::CPEDef::dfRandomFrame)    ? " RandomFrame" : "",
        def->m_Flags.is(PS::CPEDef::dfRandomPlayback) ? " RandomPlayback" : "",
        def->m_Flags.is(PS::CPEDef::dfAlignToPath)    ? " AlignToPath" : "",
        def->m_Flags.is(PS::CPEDef::dfWorldAlign)     ? " WorldAlign" : "",
        def->m_Flags.is(PS::CPEDef::dfFaceAlign)      ? " FaceAlign" : "",
        def->m_Flags.is(PS::CPEDef::dfVelocityScale)  ? " VelocityScale" : "",
        def->m_Flags.is(PS::CPEDef::dfCollision)      ? " Collision" : "",
        def->m_Flags.is(PS::CPEDef::dfCollisionDel)   ? " CollisionDel" : "",
        def->m_Flags.is(PS::CPEDef::dfTimeLimit)      ? " TimeLimit" : "");
    if (def->m_Flags.is(PS::CPEDef::dfAlignToPath))
        Msg("  APDefaultRotation (%.3f %.3f %.3f)",
            def->m_APDefaultRotation.x, def->m_APDefaultRotation.y, def->m_APDefaultRotation.z);
    if (def->m_Flags.is(PS::CPEDef::dfVelocityScale))
        Msg("  velocityScale (%.3f %.3f %.3f)",
            def->m_VelocityScale.x, def->m_VelocityScale.y, def->m_VelocityScale.z);
    if (def->m_Flags.is(PS::CPEDef::dfFramed))
        Msg("  frame: texSize (%.4f %.4f) dimX %d count %d speed %.2f",
            def->m_Frame.m_fTexSize.x, def->m_Frame.m_fTexSize.y,
            def->m_Frame.m_iFrameDimX, def->m_Frame.m_iFrameCount, def->m_Frame.m_fSpeed);
    if (def->m_Flags.is(PS::CPEDef::dfTimeLimit))
        Msg("  timeLimit %.2fs", def->m_fTimeLimit);

    Program prog{};
    float   rate = 0.f;
    TexDesc td{};
    u32     maxP = 0;
    if (!TranslateEffect(name, prog, rate, td, maxP)) { Msg("[VK GP]   (translate FAILED — see above)"); return; }

    auto dumpDom = [](const char* label, const GenDomain& d) {
        Msg("  %s: dtype %u p1(%.3f %.3f %.3f) p2(%.3f %.3f %.3f) u(%.2f %.2f %.2f) v(%.2f %.2f %.2f) r(%.3f %.3f)",
            label, d.dtype,
            d.p1[0], d.p1[1], d.p1[2], d.p2[0], d.p2[1], d.p2[2],
            d.u[0], d.u[1], d.u[2], d.v[0], d.v[1], d.v[2],
            d.radii[0], d.radii[1]);
    };
    Msg("  program: rate %.1f/s life %.2fs actions %u progFlags 0x%x (bit0=additive)",
        prog.emitRate, prog.emit.sc[3], prog.actionCount, prog.progFlags);
    Msg("  emit scalars: alpha %.2f age %.2f sigma %.2f", prog.emit.sc[0], prog.emit.sc[1], prog.emit.sc[2]);
    dumpDom("posDom  ", prog.emit.posDom);
    dumpDom("velDom  ", prog.emit.velDom);
    dumpDom("sizeDom ", prog.emit.sizeDom);
    dumpDom("colorDom", prog.emit.colorDom);
    dumpDom("rotDom  ", prog.emit.rotDom);
    for (u32 i = 0; i < prog.actionCount && i < kMaxActions; ++i) {
        const ActionRec& a = prog.actions[i];
        Msg("  action[%u]: type %u%s a(%.3f %.3f %.3f %.3f) b(%.3f %.3f %.3f %.3f) c(%.3f %.3f %.3f %.3f)",
            i, a.atype, a.atype == 0xFFFFFFFFu ? " (not interpreted)" : "",
            a.a[0], a.a[1], a.a[2], a.a[3],
            a.b[0], a.b[1], a.b[2], a.b[3],
            a.c[0], a.c[1], a.c[2], a.c[3]);
    }
    Msg("[VK GP] ===== end gp_debug =====");
}

}}  // namespace VK::GPUParticles

#undef FBasicVisualH
