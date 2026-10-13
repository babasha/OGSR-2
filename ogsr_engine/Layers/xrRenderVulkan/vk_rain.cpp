// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — rain + thunderbolt. See vk_rain.h.
//
// vkRainRender::Calculate is a line-for-line port of dxRainRender::Calculate
// (drop physics / wrap / renew / Hit). Render builds the drop streak quads and
// the splash detail-model triangles into a per-frame host ring; the lightning
// model + gradients (dxThunderboltRender port) append to the same ring. All
// draws go through the particle pass's pipelines (PBM_BLEND for rain,
// PBM_ADD for lightning) inside the "Rain" pass, after Particles.

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_env_snapshot.h"
#include <thread>                  // Rain.h: max_desired_items uses hardware_concurrency
#include <algorithm>               // std::clamp
#include "vk_rain.h"
#include "vk_Particles.h"          // EParticleBlendMode
#include "vk_pass_particles.h"     // ParticlePass::GetPipeline/GetLayout/GetTextureSet
#include "vk_pass_ssao.h"          // DeriveProjTerms (camera basis — RV().vCameraRight unverified on VK)
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_R_Backend.h"          // CBackend (pass-through reference only)
#include "HW_Vulkan.h"

#include "../xrRender/FVF.h"
#include "../../xrCDB/Frustum.h"
#include "../../xr_3da/device.h"
#include "../../xr_3da/IGame_Level.h"
#include "../../xr_3da/IGame_Persistent.h"
#include "../../xr_3da/Environment.h"
#include "../../xr_3da/Rain.h"
#include "../../xr_3da/thunderbolt.h"

// Console knobs (vk_console_min.cpp — same vars the DX renderers use).
extern Fvector4 ps_ssfx_rain_1;        // x = drop length, y = drop width, z = speed mult
extern float    ps_r2_no_details_radius;
extern float    ps_r2_no_rain_radius;  // splash-free radius around the camera
extern int      ps_r_rain_debug;       // r_rain_debug — solid red streaks (geometry vs texture triage)
extern int      ps_r_rain_enable;      // r_rain — master rain on/off
extern int      ps_r_rain_drops;       // r_rain_drops — max drops (0 = stock max_desired_items)
extern int      ps_r_rain_rq_static;   // r_rain_rq_static — drop rays against the static level only
extern int      ps_r_rain_hcache;      // r_rain_hcache — those static rays from a 0.5 m height cache (CEffect_Rain)
extern float    ps_r_rain_sun;         // r_rain_sun — forward-scatter lobe on the sun/moon/lightning direction

namespace {

// ---------------------------------------------------------------------------
// Detail model (.dm) — CPU-side mirror of CDetail::Load (DetailModel.cpp)
// minus the shader/VB/IB creation. Keeps the texture NAME for the particle
// texture cache and the raw verts/indices for per-frame transfer.
// ---------------------------------------------------------------------------
struct vkDM
{
    struct VertIn { Fvector P; float u, v; };

    xr_vector<VertIn> verts;
    xr_vector<u16>    indices;
    shared_str        texture;     // second string in the file (fnT)
    Fsphere           bv_sphere{};
    Fbox              bv_bb{};

    bool Load(IReader* F)
    {
        string256 fnS, fnT;
        F->r_stringZ(fnS, sizeof(fnS));   // shader name — unused (blend mode is fixed per consumer)
        F->r_stringZ(fnT, sizeof(fnT));
        texture = fnT;

        F->r_u32();      // flags
        F->r_float();    // min scale
        F->r_float();    // max scale

        const u32 nV = F->r_u32();
        const u32 nI = F->r_u32();
        if (!nV || !nI || (nI % 3))
            return false;

        verts.resize(nV);
        F->r(verts.data(), nV * sizeof(VertIn));
        indices.resize(nI);
        F->r(indices.data(), nI * sizeof(u16));

        bv_bb.invalidate();
        for (const auto& v : verts)
            bv_bb.modify(const_cast<Fvector&>(v.P));
        bv_bb.getsphere(bv_sphere.P, bv_sphere.R);
        return true;
    }

    // Transform + index-expand straight into the ring (non-indexed triangle
    // list — counts are tiny, splash model is a handful of tris).
    void Emit(const Fmatrix& xform, u32 C, float dv, FVF::LIT* out) const
    {
        for (size_t i = 0; i < indices.size(); ++i)
        {
            const VertIn& src = verts[indices[i]];
            Fvector P;
            xform.transform_tiny(P, src.P);
            out[i].set(P, C, src.u, src.v + dv);
        }
    }
    u32 EmitCount() const { return (u32)indices.size(); }
};

vkDM* LoadDM(const char* fsRoot, const char* name)
{
    IReader* F = FS.r_open(fsRoot, name);
    if (!F) {
        Msg("![VK Rain] can't open detail model '%s'", name);
        return nullptr;
    }
    vkDM* dm = xr_new<vkDM>();
    const bool ok = dm->Load(F);
    FS.r_close(F);
    if (!ok) { xr_delete(dm); Msg("![VK Rain] bad detail model '%s'", name); return nullptr; }
    return dm;
}

// ---------------------------------------------------------------------------
// Shared per-frame vertex ring + draw chunks. Filled by vkRainRender /
// vkThunderboltRender while Pass_Rain runs, drawn once at the end.
// ---------------------------------------------------------------------------
constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
constexpr u32 kVtxStride      = sizeof(FVF::LIT);                  // 24
constexpr VkDeviceSize kRingBytes = 2 * 1024 * 1024;               // ~87k verts/frame
constexpr u32 kRingVerts = (u32)(kRingBytes / kVtxStride);

// kind: 0 = generic blend-mode pipeline (thunderbolt), 1 = rain splash
// (tex-alpha shape), 2 = rain drop streak (procedural shape).
struct DrawChunk { shared_str tex; EParticleBlendMode mode; u32 first; u32 count; u8 kind; };

VK::CVulkanBuffer s_Ring[kFramesInFlight];
bool              s_RingInit = false;
u8*               s_RingBase = nullptr;   // mapped while Pass_Rain runs
// Two-slot pipeline: what Rain_Extract (main) builds for a frame -- the chunk list, how much of the ring it used, the
// "built for frame N" tag -- lives in that frame's slot, so extract of N+1 never clears the list Pass_Rain(N) is walking.
struct RainSlot { xr_vector<DrawChunk> chunks; u32 vtxUsed = 0; u32 frame = u32(-1); bool raining = false; };
RT::PerSlot<RainSlot> s_rainSlots;
inline RainSlot& RS() { return s_rainSlots.cur(); }
bool              s_ExtractOn = false;    // true only while Rain_Extract runs (what s_Ctx used to say)
// 10-10 [VK Rain] cpu: the main thread paid +3.3 ms a frame in the rain on the Switch (v309: neither r_rain_rq_static nor
// r_rain_drops moved it) -- where it goes, summed over 300 frames.
u64 s_tCalc = 0, s_tDrops = 0, s_tSplash = 0, s_tBolt = 0, s_tMap = 0;
u32 s_nRays = 0, s_nBorn = 0, s_nSplash = 0, s_nFrames = 0, s_nItems = 0, s_nProbe = 0;   // s_nProbe: height-cache misses (vertical rays)

FVF::LIT* RingAlloc(u32 count, u32& first)
{
    if (!s_RingBase || RS().vtxUsed + count > kRingVerts)
        return nullptr;
    first = RS().vtxUsed;
    RS().vtxUsed += count;
    return (FVF::LIT*)(s_RingBase + (size_t)first * kVtxStride);
}

void Commit(const shared_str& tex, EParticleBlendMode mode, u32 first, u32 count, u8 kind = 0)
{
    if (count)
        RS().chunks.push_back({ tex, mode, first, count, kind });
}

// CEffect_Rain::Render / CEffect_Thunderbolt::Render take a CBackend& they
// only forward; nothing on this path reads it. Raw storage — no ctor/dtor.
CBackend& DummyBackend()
{
    alignas(CBackend) static char storage[sizeof(CBackend)]{};
    return *reinterpret_cast<CBackend*>(storage);
}

}  // namespace

// ===========================================================================
// vkRainRender — dxRainRender port
// ===========================================================================
class vkRainRender final : public IRainRender
{
public:
    vkRainRender()
    {
        m_DM = LoadDM(fsgame::game_meshes, "dm\\rain.dm");   // splash model ("капля")
        m_DropBounds.P.set(0, 0, 0);
        m_DropBounds.R = 0.f;
        if (m_DM)
            m_DropBounds = m_DM->bv_sphere;
    }
    ~vkRainRender() override
    {
        if (m_DM && m_OwnsDM)
            xr_delete(m_DM);
    }

    void Copy(IRainRender& _in) override
    {
        // FactoryPtr copy semantics: share the (immutable) splash model.
        vkRainRender* src = static_cast<vkRainRender*>(&_in);
        if (m_DM && m_OwnsDM)
            xr_delete(m_DM);
        m_DM = src->m_DM;
        m_OwnsDM = false;
        m_DropBounds = src->m_DropBounds;
    }

    const Fsphere& GetDropBounds() const override { return m_DropBounds; }

    // ----- simulation (dxRainRender::Calculate, verbatim minus stats) -------
    void Calculate(CEffect_Rain& owner) override
    {
        const float factor = g_pGamePersistent->Environment().CurrentEnv->rain_density;
        if (factor < EPS_L)
            return;

        owner.hcache = ps_r_rain_hcache != 0;   // 10-10: the static rays from the height cache (CEffect_Rain::RayPickCached)
        const float _drop_speed = ps_ssfx_rain_1.z;
        // 10-10: r_rain_drops caps the count (every drop wrapping round the camera casts a ray, main thread)
        const float maxItems = ps_r_rain_drops > 0 ? float(ps_r_rain_drops) : max_desired_items;
        const float minItems = ps_r_rain_drops > 0 ? maxItems / 10.f : min_desired_items;
        const size_t desired_items = iFloor(minItems + (factor * (maxItems - minItems)));

        constexpr float b_radius_wrap_sqr = _sqr(source_radius * 1.5f);

        owner.items.reserve(desired_items);
        while (owner.items.size() < desired_items)
        {
            auto& one = owner.items.emplace_back();
            ++s_nBorn, owner.Born(one, source_radius, _drop_speed, ps_r_rain_rq_static ? collide::rqtStatic : collide::rqtBoth);
        }
        // r_rain_drops lowered mid-rain: drop the excess (stock keeps them flying until the density falls)
        if (ps_r_rain_drops > 0 && owner.items.size() > size_t(maxItems))
            owner.items.resize(size_t(maxItems));
        s_nItems = (u32)owner.items.size();
        s_nProbe += owner.hprobes;   // 10-10: the height cache's misses so far this frame (births above)
        owner.hprobes = 0;

        // build source plane
        Fplane src_plane;
        constexpr Fvector norm{0.f, -1.f, 0.f};
        Fvector upper;
        upper.set(RV().vCameraPosition.x, RV().vCameraPosition.y + source_offset, RV().vCameraPosition.z);
        src_plane.build(upper, norm);

        const Fvector& vEye = RV().vCameraPosition;
        u32 total_cnt = (u32)owner.items.size();

        for (auto& one : owner.items)
        {
            if (one.dwTime_Hit < RV().dwTimeGlobal)
                owner.Hit(one.Phit);

            if (one.dwTime_Life < RV().dwTimeGlobal)
            {
                if (total_cnt <= desired_items)
                    ++s_nBorn, owner.Born(one, source_radius, _drop_speed, ps_r_rain_rq_static ? collide::rqtStatic : collide::rqtBoth);
                else
                    total_cnt--;
            }

            const float dt = RV().fTimeDelta;
            one.P.mad(one.D, one.fSpeed * dt);

            Fvector wdir;
            wdir.set(one.P.x - vEye.x, 0, one.P.z - vEye.z);
            float wlen = wdir.square_magnitude();
            if (wlen > b_radius_wrap_sqr)
            {
                wlen = _sqrt(wlen);

                if ((one.P.y - vEye.y) < sink_offset)
                {
                    one.invalidate();   // need born
                }
                else
                {
                    Fvector inv_dir;
                    inv_dir.invert(one.D);
                    wdir.div(wlen);
                    one.P.mad(one.P, wdir, -(wlen + source_radius));

                    if (Fvector src_p; src_plane.intersectRayPoint(one.P, inv_dir, src_p))
                    {
                        const float dist_sqr = one.P.distance_to_sqr(src_p);
                        float height = max_distance;

                        ++s_nRays;
                        if (owner.RayPickCached(src_p, one.D, height, ps_r_rain_rq_static ? collide::rqtStatic : collide::rqtBoth))
                        {
                            if (_sqr(height) <= dist_sqr)
                                one.invalidate();                                       // need born
                            else
                                owner.RenewItem(one, height - _sqrt(dist_sqr), TRUE);   // fly to point
                        }
                        else
                        {
                            owner.RenewItem(one, max_distance - _sqrt(dist_sqr), FALSE);
                        }
                    }
                    else
                    {
                        one.invalidate();   // need born
                    }
                }
            }
        }
    }

    // ----- visualization (dxRainRender::Render over the ring) ---------------
    void Render(CBackend& /*unused*/, CEffect_Rain& owner) override
    {
        if (!s_ExtractOn || !s_RingBase)
            return;

        const float factor = g_pGamePersistent->Environment().CurrentEnv->rain_density;
        if (factor < EPS_L || owner.items.empty())
            return;

        const float _drop_len   = ps_ssfx_rain_1.x;
        const float _drop_width = ps_ssfx_rain_1.y;

        const float factor_visual = factor / 2.f + .5f;
        // ssfx_rain_1.y is tuned for the SSFX refraction shader (0.02 m — the
        // refraction makes sub-pixel streaks visible). Our plain alpha quads
        // need ≥ ~1.5 px on screen or they vanish: widen with distance.
        const float pixAngle = (RV().dwHeight > 0)
            ? deg2rad(RV().fFOV) / float(RV().dwHeight) : 0.0012f;
        const float minWidthPerMeter = pixAngle * 0.9f;   // ~1 px: thin streaks, not "tracers"
        // Colours. SSFX drop output = refracted background + hemi×brightness at
        // alpha 0.4 — the NET effect over the backdrop is a faint hemi-tinted
        // glint. The drop pipeline is ADDITIVE (PBM_ALPHA_ADD), so the vertex
        // colour is ONLY that lift: hemi × brightness; no fog paint-over (that
        // alpha-blend version read as a downpour of tracers vs R4's drizzle).
        const auto* E = g_pGamePersistent->Environment().CurrentEnv;
        // DIRECTIONAL term (r_rain_sun). Hemi alone is flat-lit rain, and it is also
        // rain a thunderbolt cannot reach: the bolt boosts sun/sky/fog colour and
        // points sun_dir at the strike (thunderbolt.cpp:314-325), and hemi is the one
        // field it never writes — so the whole downpour ignored every flash.
        // A falling drop is a water cylinder: it scatters FORWARD, so a downpour
        // lights up when the light is BEHIND it (you look toward the sun/moon/bolt)
        // and goes near-black when the light is behind YOU. That is one lobe on the
        // sun direction, and it delivers the lightning flash through the same path —
        // the bolt swings sun_dir at the camera's part of the sky, so a strike ahead
        // of the player rips through the rain while one behind him only glows.
        // A floor is kept (0.12) so drops never fully vanish side-on.
        Fvector sunD = E->sun_dir;
        sunD.normalize_safe();
        const float fwd = -RV().vCameraDirection.dotproduct(sunD);   // +1 = looking INTO the light
        const float g = _max(0.f, fwd);
        const float lobe = ps_r_rain_sun * (0.12f + 0.88f * g * g * g);
        u32 u_drop_color = color_rgba_f(_min(E->hemi_color.x * 0.6f + E->sun_color.x * lobe, 1.f),
                                        _min(E->hemi_color.y * 0.6f + E->sun_color.y * lobe, 1.f),
                                        _min(E->hemi_color.z * 0.6f + E->sun_color.z * lobe, 1.f),
                                        factor_visual * 0.32f);
        // Splashes stay alpha-blended (their texture alpha works): fog+hemi mix.
        const u32 u_splash_color = color_rgba_f(_min(E->fog_color.x + E->hemi_color.x * 0.4f, 1.f),
                                                _min(E->fog_color.y + E->hemi_color.y * 0.4f, 1.f),
                                                _min(E->fog_color.z + E->hemi_color.z * 0.4f, 1.f),
                                                factor_visual * 0.5f);
        if (ps_r_rain_debug)
            u_drop_color = color_rgba(255, 32, 32, 255);   // alpha 1.0 → solid-quad debug path

        CFrustum view;
        Fmatrix vp = RV().mFullTransform;
        view.CreateFromMatrix(vp, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);

        const Fvector& vEye = RV().vCameraPosition;

        // --- drop streaks: camera-facing quads, 6 verts each ---
        u32 first = 0;
        FVF::LIT* out = RingAlloc((u32)owner.items.size() * 6, first);
        if (!out)
            return;
        u32 vCount = 0;

        for (auto& one : owner.items)
        {
            Fvector& pos_head = one.P;
            Fvector pos_trail;
            pos_trail.mad(pos_head, one.D, -_drop_len * factor_visual);

            // Culling
            Fvector sC, lineD;
            float sR;
            sC.sub(pos_head, pos_trail);
            lineD.normalize(sC);
            sC.mul(.5f);
            sR = sC.magnitude();
            sC.add(pos_trail);

            if (!view.testSphere_dirty(sC, sR))
                continue;

            constexpr Fvector2 UV[2][4]{{{0, 1}, {0, 0}, {1, 1}, {1, 0}}, {{1, 0}, {1, 1}, {0, 0}, {0, 1}}};

            Fvector P0, P1, P2, P3, lineTop, camDir;
            camDir.sub(sC, vEye);
            const float dist = camDir.magnitude();
            camDir.div(_max(dist, 0.01f));
            lineTop.crossproduct(camDir, lineD);
            const float w = _max(_drop_width, dist * minWidthPerMeter);
            const u32 s = one.uv_set;
            P0.mad(pos_trail, lineTop, -w);
            P1.mad(pos_trail, lineTop, w);
            P2.mad(pos_head, lineTop, -w);
            P3.mad(pos_head, lineTop, w);
            // quad (P0,P1,P2,P3) → two tris matching the QuadIB winding
            out[vCount + 0].set(P0, u_drop_color, UV[s][0].x, UV[s][0].y);
            out[vCount + 1].set(P1, u_drop_color, UV[s][1].x, UV[s][1].y);
            out[vCount + 2].set(P2, u_drop_color, UV[s][2].x, UV[s][2].y);
            out[vCount + 3].set(P2, u_drop_color, UV[s][2].x, UV[s][2].y);
            out[vCount + 4].set(P1, u_drop_color, UV[s][1].x, UV[s][1].y);
            out[vCount + 5].set(P3, u_drop_color, UV[s][3].x, UV[s][3].y);
            vCount += 6;
        }
        RS().vtxUsed -= (u32)owner.items.size() * 6 - vCount;   // return the unused tail
        const u64 tDropsEnd = CPU::QPC_us();
        Commit("fx\\fx_rain", PBM_BLEND, first, vCount, /*kind*/ 2);

        // --- splash particles (detail model per active particle) ---
        if (!m_DM || !owner.particle_active)
            return;

        const float dt = RV().fTimeDelta;
        const u32 dmVerts = m_DM->EmitCount();
        Fmatrix mXform, mScale;
        u32 splashFirst = 0, splashCount = 0;

        CEffect_Rain::Particle* P = owner.particle_active;
        while (P)
        {
            CEffect_Rain::Particle* next = P->next;

            P->time -= dt;
            if (P->time < 0)
            {
                owner.p_free(P);
                P = next;
                continue;
            }

            if (view.testSphere_dirty(P->bounds.P, P->bounds.R)
                && (fis_zero(ps_r2_no_details_radius) || RV().vCameraPosition.distance_to(P->bounds.P) > ps_r2_no_rain_radius))
            {
                u32 f = 0;
                FVF::LIT* dst = RingAlloc(dmVerts, f);
                if (dst)
                {
                    if (!splashCount)
                        splashFirst = f;
                    const float scale = P->time / particles_time;
                    mScale.scale(scale, scale, scale);
                    mXform.mul_43(P->mXForm, mScale);
                    m_DM->Emit(mXform, u_splash_color, 0.f, dst);
                    splashCount += dmVerts;
                }
            }
            P = next;
        }
        Commit(m_DM->texture, PBM_BLEND, splashFirst, splashCount, /*kind*/ 1);
        s_tSplash += CPU::QPC_us() - tDropsEnd;
        s_nSplash += dmVerts ? splashCount / dmVerts : 0;

        static bool s_diag = false;
        if (!s_diag && vCount) {
            s_diag = true;
            Msg("[VK Rain] first frame drawn: drops=%u splashes=%u (factor %.2f)", vCount / 6, splashCount ? splashCount / dmVerts : 0, factor);
        }
    }

private:
    vkDM*   m_DM = nullptr;
    bool    m_OwnsDM = true;
    Fsphere m_DropBounds;
};

// ===========================================================================
// Thunderbolt — dxThunderboltRender / dxThunderboltDescRender / dxFlareRender
// ===========================================================================
class vkFlareRender final : public IFlareRender
{
public:
    shared_str m_Texture;

    void Copy(IFlareRender& _in) override { m_Texture = static_cast<vkFlareRender*>(&_in)->m_Texture; }
    void CreateShader(LPCSTR /*sh*/, LPCSTR tex) override { m_Texture = tex; }
    void DestroyShader() override { m_Texture = nullptr; }
};

class vkThunderboltDescRender final : public IThunderboltDescRender
{
public:
    vkDM* l_model = nullptr;

    ~vkThunderboltDescRender() override { DestroyModel(); }

    void Copy(IThunderboltDescRender& _in) override { l_model = static_cast<vkThunderboltDescRender*>(&_in)->l_model; }

    void CreateModel(LPCSTR m_name) override
    {
        DestroyModel();
        l_model = LoadDM(fsgame::game_meshes, m_name);
    }
    void DestroyModel() override { xr_delete(l_model); }
};

class vkThunderboltRender final : public IThunderboltRender
{
public:
    void Copy(IThunderboltRender&) override {}

    void Render(CBackend& /*unused*/, CEffect_Thunderbolt& owner) override
    {
        if (!s_ExtractOn || !s_RingBase || !owner.current)
            return;

        // Camera basis: RV().vCameraRight/Top are not maintained on the VK
        // path — derive from the matrix that renders the frame.
        const VK::ProjTerms pt = VK::DeriveProjTerms(RV().mFullTransform);

        // --- lightning model (UV v-anim like dxThunderboltRender) ---
        float dv = owner.lightning_phase * 0.5f;
        dv = (owner.lightning_phase > 0.5f) ? Random.randI(2) * 0.5f : dv;

        const vkThunderboltDescRender* desc = static_cast<vkThunderboltDescRender*>(&*owner.current->m_pRender);
        if (desc && desc->l_model)
        {
            vkDM* dm = desc->l_model;
            u32 first = 0;
            FVF::LIT* out = RingAlloc(dm->EmitCount(), first);
            if (out)
            {
                dm->Emit(owner.current_xform, 0xffffffff, dv, out);
                Commit(dm->texture, PBM_ADD, first, dm->EmitCount());
            }
        }

        // --- gradients: two camera-facing quads (top at the bolt origin,
        //     center at the flash point), additive like the DX sun shader ---
        const SThunderboltDesc::SFlare* gradients[2] = { owner.current->m_GradientTop, owner.current->m_GradientCenter };
        const Fvector centers[2] = { owner.current_xform.c, owner.lightning_center };
        for (int g = 0; g < 2; ++g)
        {
            const auto* G = gradients[g];
            if (!G)
                continue;
            // R4 uses the TOP gradient's opacity for both quads — kept as-is.
            const u32 c_val = std::clamp(iFloor(owner.current->m_GradientTop->fOpacity * owner.lightning_phase * 255.f), 0, 255);
            const u32 c = color_rgba(c_val, c_val, c_val, c_val);

            const vkFlareRender* flare = static_cast<vkFlareRender*>(&*G->m_pFlare);
            if (!flare || !flare->m_Texture.size())
                continue;

            Fvector vecSx, vecSy;
            vecSx.mul(pt.right, G->fRadius.x * owner.lightning_size);
            vecSy.mul(pt.top, -G->fRadius.y * owner.lightning_size);

            const Fvector& C0 = centers[g];
            Fvector q[4];
            q[0].set(C0.x + vecSx.x - vecSy.x, C0.y + vecSx.y - vecSy.y, C0.z + vecSx.z - vecSy.z);
            q[1].set(C0.x + vecSx.x + vecSy.x, C0.y + vecSx.y + vecSy.y, C0.z + vecSx.z + vecSy.z);
            q[2].set(C0.x - vecSx.x - vecSy.x, C0.y - vecSx.y - vecSy.y, C0.z - vecSx.z - vecSy.z);
            q[3].set(C0.x - vecSx.x + vecSy.x, C0.y - vecSx.y + vecSy.y, C0.z - vecSx.z + vecSy.z);

            constexpr Fvector2 uv[4]{{0, 0}, {0, 1}, {1, 0}, {1, 1}};
            u32 first = 0;
            FVF::LIT* out = RingAlloc(6, first);
            if (!out)
                continue;
            out[0].set(q[0], c, uv[0].x, uv[0].y);
            out[1].set(q[1], c, uv[1].x, uv[1].y);
            out[2].set(q[2], c, uv[2].x, uv[2].y);
            out[3].set(q[2], c, uv[2].x, uv[2].y);
            out[4].set(q[1], c, uv[1].x, uv[1].y);
            out[5].set(q[3], c, uv[3].x, uv[3].y);
            Commit(flare->m_Texture, PBM_ADD, first, 6);
        }

        static bool s_diag = false;
        if (!s_diag) { s_diag = true; Msg("[VK Rain] first thunderbolt drawn"); }
    }
};

// ===========================================================================
// Pass
// ===========================================================================
namespace VK {

// RENDER_THREAD_PLAN step 8: the simulation tick (eff_Rain->Calculate: drop physics, ray-picks, splash spawn) and the
// vertex build touch live game state, so they run here, in the PREPARE phase. Pass_Rain only draws the chunks.

void Rain_Extract()
{
    RT_ASSERT_MAIN();
    RS().chunks.clear(); RS().vtxUsed = 0; RS().frame = u32(-1); RS().raining = false;
    if (!ps_r_rain_enable) return;   // master off (r_rain 0): no drops/splashes/thunderbolt
    if (!ParticlePass::Ready() || !g_pGameLevel || !g_pGamePersistent) return;

    CEnvironment& env = g_pGamePersistent->Environment();
    if (!env.eff_Rain || !env.eff_Thunderbolt)
        return;

    // Cheap out: nothing falling, nothing flashing -> skip the ring map.
    const bool raining = env.CurrentEnv && env.CurrentEnv->rain_density > EPS_L;

    if (!s_RingInit) {
        for (u32 i = 0; i < kFramesInFlight; ++i)
            s_Ring[i].Create(kRingBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
        s_RingInit = true;
    }
    CVulkanBuffer& ring = s_Ring[RV().ringSlot];
    if (!ring.IsValid())
        return;

    s_ExtractOn = true;
    const u64 t0 = CPU::QPC_us();
    s_RingBase = (u8*)ring.Map();

    if (s_RingBase)
    {
        u64 t1 = CPU::QPC_us(), t2 = t1, t3 = t1;
        if (raining)
        {
            env.eff_Rain->Calculate();
            t2 = CPU::QPC_us();
            env.eff_Rain->Render(DummyBackend());
            t3 = CPU::QPC_us();
        }
        env.eff_Thunderbolt->Render(DummyBackend());   // gated on stWorking inside
        const u64 t4 = CPU::QPC_us();
        ring.Flush();
        ring.Unmap();
        s_RingBase = nullptr;
        if (raining)
        {
            s_tCalc += t2 - t1;
            s_tDrops += t3 - t2;   // drops + splashes; the splashes alone in s_tSplash
            s_tBolt += t4 - t3;
            s_tMap += (t1 - t0) + (CPU::QPC_us() - t4);
            if (++s_nFrames >= 300)
            {
                const float k = 1.f / (1000.f * float(s_nFrames));
                Msg("[VK Rain] cpu per frame: calc %.2f ms (%.0f births + %.0f wrap rays, %.1f height-cache rays) | drops %.2f | splashes %.2f (%.0f) | bolt %.2f | map %.2f | %u drops",
                    float(s_tCalc) * k, float(s_nBorn) / float(s_nFrames), float(s_nRays) / float(s_nFrames), float(s_nProbe) / float(s_nFrames), float(s_tDrops - s_tSplash) * k, float(s_tSplash) * k,
                    float(s_nSplash) / float(s_nFrames), float(s_tBolt) * k, float(s_tMap) * k, s_nItems);
                s_tCalc = s_tDrops = s_tSplash = s_tBolt = s_tMap = 0;
                s_nRays = s_nBorn = s_nSplash = s_nFrames = s_nProbe = 0;
            }
        }
    }
    s_ExtractOn = false;
    RS().frame   = RV().frameIndex;
    RS().raining = raining;
}

void Pass_Rain(FrameContext& ctx)
{
    if (!ps_r_rain_enable) return;
    if (ctx.cmd == VK_NULL_HANDLE || !ctx.viewProj || !ParticlePass::Ready()
        || !g_pGameLevel || !g_pGamePersistent)
        return;
    if (RS().frame != RV().frameIndex || !s_RingInit)   // built by Rain_Extract for THIS frame, or nothing
        return;
    CVulkanBuffer& ring = s_Ring[CommandManager.GetCurrentFrame()];
    if (!ring.IsValid())
        return;

    if (!RS().chunks.empty())
    {
        VkPipelineLayout layout = ParticlePass::GetLayout();

        // Colour LOAD + depth LOAD (test, no write) — the shared overlay contract.
        VK::BeginOverlayRendering(ctx.cmd, ctx, VK_ATTACHMENT_STORE_OP_DONT_CARE);

        // ⚠ VERTEX|FRAGMENT, not VERTEX alone. The particle layout declares its range over
    // BOTH stages, and a push must name EVERY stage of every range it overlaps — a
    // subset is invalid (VUID-vkCmdPushConstants-offset-01796). Only reproduces in
    // weather that actually draws, which is why it hid until a rainy validation run.
    vkCmdPushConstants(ctx.cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(Fmatrix), ctx.viewProj);
        VkBuffer vbuf = ring.GetHandle();
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &vbuf, &voff);

        VkPipeline lastPipe = VK_NULL_HANDLE;
        for (const DrawChunk& ch : RS().chunks)
        {
            VkDescriptorSet set = ParticlePass::GetTextureSetAsync(ch.tex.c_str(), nullptr);   // a first use loads behind the frame
            if (set == VK_NULL_HANDLE) {
                // Once per name: a silently skipped chunk looks exactly like
                // "drops don't render" — make the cause visible in the log.
                static xr_vector<shared_str> s_warned;
                if (std::find(s_warned.begin(), s_warned.end(), ch.tex) == s_warned.end()) {
                    s_warned.push_back(ch.tex);
                    Msg("![VK Rain] texture set NULL for '%s' — chunk skipped", ch.tex.c_str());
                }
                continue;
            }
            VkPipeline want = (ch.kind == 2) ? ParticlePass::GetRainDropPipeline()
                            : (ch.kind == 1) ? ParticlePass::GetRainPipeline()
                                             : ParticlePass::GetPipeline(ch.mode);
            if (want == VK_NULL_HANDLE)
                continue;
            if (want != lastPipe) { vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want); lastPipe = want; }
            vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
            vkCmdDraw(ctx.cmd, ch.count, 1, ch.first, 0);
        }

        vkCmdEndRendering(ctx.cmd);
    }

    // Periodic state line while raining (~every 10 s) -- enough to diagnose
    // "no drops / no wetness" reports from the log alone, no debug flags.
    if (RS().raining && ES().CurrentEnv) {
        static u32 s_stateCd = 0;
        if (s_stateCd == 0) {
            s_stateCd = 600;
            Msg("[VK Rain] state: density=%.2f envWet=%.3f chunks=%u verts=%u",
                ES().CurrentEnv->rain_density, ES().wetness_factor, (u32)RS().chunks.size(), RS().vtxUsed);
        } else --s_stateCd;
    }
}

void RainPass_Destroy()
{
    if (s_RingInit)
    {
        for (u32 i = 0; i < kFramesInFlight; ++i)
            s_Ring[i].Destroy();
        s_RingInit = false;
    }
    for (RainSlot& s : s_rainSlots.v) s.chunks.clear();
}

}  // namespace VK

// ===========================================================================
// Factory creators
// ===========================================================================
IRainRender*            VK_CreateRainRender()            { return xr_new<vkRainRender>(); }
IThunderboltRender*     VK_CreateThunderboltRender()     { return xr_new<vkThunderboltRender>(); }
IThunderboltDescRender* VK_CreateThunderboltDescRender() { return xr_new<vkThunderboltDescRender>(); }
IFlareRender*           VK_CreateFlareRender()           { return xr_new<vkFlareRender>(); }
