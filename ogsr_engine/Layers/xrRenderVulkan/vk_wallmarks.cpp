// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan — static wallmarks. See vk_wallmarks.h. The geometry build
// (BuildMatrix / RecurseTri / the box query + adjacency walk) is a line-for-
// line port of CWallmarksEngine; rendering replaces RCache streaming with a
// per-frame host-visible vertex ring drawn through the particle pass's
// PBM_BLEND pipeline (FVF::LIT vertices, same layout, same texture cache).

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_wallmarks.h"
#include "../../xrCore/load_prof.h" // [rt prof]: wm.static / wm.skel
#include "vk_Particles.h"          // EParticleBlendMode (PBM_BLEND)
#include "vk_pass_particles.h"     // ParticlePass::GetPipeline/GetLayout/GetTextureSet
#include "vk_buffer.h"
#include "vk_command_buffer.h"     // CommandManager.GetCurrentFrame() / FRAMES_IN_FLIGHT
#include "HW_Vulkan.h"

#include "../xrRender/FVF.h"
#include "../../xrCDB/xrXRC.h"     // xrXRC box query (pulls xrCDB.h: TRI/Collector)
#include "../../xrCDB/Frustum.h"   // CFrustum / sPoly clipper
#include "../../xr_3da/device.h"   // Device.vCameraPosition / fTimeDelta
#include "../../xr_3da/IGame_Level.h"  // g_pGameLevel->ObjectSpace (static model)

#include <algorithm>

// Console knobs (vk_console_min.cpp — same vars the DX renderers use).
extern float ps_r__WallmarkTTL;    // seconds, default 60
extern float ps_r__ssaDISCARD;     // screen-space-area discard (we clip at /4 like R4)

// Skeleton wallmark bridges (vk_SkeletonCustom.cpp — blood decals on NPCs,
// queued per frame by CKinematics::CalculateWallmarks; the Fill call CPU-skins
// the decal's faces against the current pose into FVF::LIT verts).
extern u32  VK_SkelWM_Count();
extern const char* VK_SkelWM_Texture(u32 i);
extern u32  VK_SkelWM_VCount(u32 i);
extern void VK_SkelWM_Fill(u32 i, FVF::LIT*& V);
extern void VK_SkelWM_EndFrame();

namespace VK {
namespace Wallmarks {

namespace {

constexpr u32 kFramesInFlight = CVulkanCommandManager::FRAMES_IN_FLIGHT;
constexpr u32 kVtxStride      = sizeof(float) * 3 + sizeof(u32) + sizeof(float) * 2;  // FVF::LIT, 24
constexpr VkDeviceSize kRingBytes = 1 * 1024 * 1024;            // ~43k verts/frame — plenty
constexpr u32 kRingVerts = (u32)(kRingBytes / kVtxStride);

// Z-fighting: R4 shifts the projection matrix (proj._43 -= WallmarkSHIFT). On
// the VK path Device.mProject is IDENTITY (lesson: never read it), so instead
// the wallmark VERTICES are pushed off the surface along its normal at build
// time — same visual result, no matrix surgery.
constexpr float kNormalShift = 0.003f;   // 3 mm

struct static_wallmark
{
    Fsphere             bounds;
    xr_vector<FVF::LIT> verts;
    float               ttl;
};

struct wm_slot
{
    shared_str                  texture;
    xr_vector<static_wallmark*> items;
};

xrCriticalSection            s_lock;      // guards the slots and the pool (bullets/physics add off-thread)
xr_vector<wm_slot*>          s_slots;
xr_vector<static_wallmark*>  s_pool;

// Build scratch (serialized by s_buildLock — mirrors R4's single queue consumer). Building a mark (the level triangles
// around the hit + clipping) used to run under s_lock, which the render thread's Render() also takes; now only the insert
// into a slot does. (08-10: the "Wallmarks 6-13 ms" render-thread spike was NOT this but the lazy pipelines -- Prewarm.)
xrCriticalSection            s_buildLock;
Fvector        sml_normal;
CFrustum       sml_clipper;
sPoly          sml_poly_dest;
sPoly          sml_poly_src;
CDB::Collector sml_collector;
xr_vector<u32> sml_adjacency;

CVulkanBuffer  s_Ring[kFramesInFlight];
bool           s_RingInit = false;

static_wallmark* wm_allocate()
{
    static_wallmark* W;
    if (s_pool.empty())
        W = xr_new<static_wallmark>();
    else { W = s_pool.back(); s_pool.pop_back(); }
    W->ttl = ps_r__WallmarkTTL;
    W->verts.clear();
    return W;
}

void wm_destroy(static_wallmark* W) { s_pool.push_back(W); }

void EnsureRings()
{
    if (s_RingInit) return;
    for (u32 i = 0; i < kFramesInFlight; ++i)
        s_Ring[i].Create(kRingBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    s_RingInit = true;
}

wm_slot* FindOrAppendSlot(const shared_str& tex)
{
    for (wm_slot* s : s_slots)
        if (s->texture == tex)
            return s;
    wm_slot* s = xr_new<wm_slot>();
    s->texture = tex;
    s_slots.push_back(s);
    return s;
}

// R4 CWallmarksEngine::BuildMatrix — ortho projection basis at the hit point.
void BuildMatrix(Fmatrix& mView, const float invsz, const Fvector& from)
{
    Fmatrix mScale;
    Fvector at, up, right, y;
    at.sub(from, sml_normal);
    y.set(0, 1, 0);
    if (_abs(sml_normal.y) > .99f)
        y.set(1, 0, 0);
    right.crossproduct(y, sml_normal);
    up.crossproduct(sml_normal, right);
    mView.build_camera(from, at, up);
    mScale.scale(invsz, invsz, invsz);
    mView.mulA_43(mScale);
}

// R4 CWallmarksEngine::RecurseTri — clip the collected triangles against the
// projection frustum, fan-triangulate, project UVs, flood-fill via adjacency.
void RecurseTri(const u32 t, Fmatrix& mView, static_wallmark& W)
{
    CDB::TRI* T = sml_collector.getT() + t;
    if (T->dummy)
        return;
    T->dummy = 0xffffffff;

    const u32* v_ids = T->verts;
    const Fvector* v_data = sml_collector.getV();
    sml_poly_src.clear();
    sml_poly_src.push_back(v_data[v_ids[0]]);
    sml_poly_src.push_back(v_data[v_ids[1]]);
    sml_poly_src.push_back(v_data[v_ids[2]]);
    sml_poly_dest.clear();

    sPoly* P = sml_clipper.ClipPoly(sml_poly_src, sml_poly_dest);

    if (P)
    {
        FVF::LIT V0, V1, V2;
        Fvector UV;

        mView.transform_tiny(UV, (*P)[0]);
        V0.set((*P)[0], 0, (1 + UV.x) * .5f, (1 - UV.y) * .5f);
        mView.transform_tiny(UV, (*P)[1]);
        V1.set((*P)[1], 0, (1 + UV.x) * .5f, (1 - UV.y) * .5f);

        for (u32 i = 2; i < P->size(); i++)
        {
            mView.transform_tiny(UV, (*P)[i]);
            V2.set((*P)[i], 0, (1 + UV.x) * .5f, (1 - UV.y) * .5f);
            W.verts.push_back(V0);
            W.verts.push_back(V1);
            W.verts.push_back(V2);
            V1 = V2;
        }

        for (u32 i = 0; i < 3; i++)
        {
            const u32 adj = sml_adjacency[3 * t + i];
            if (0xffffffff == adj)
                continue;
            CDB::TRI* SML = sml_collector.getT() + adj;
            v_ids = SML->verts;

            Fvector test_normal;
            test_normal.mknormal(v_data[v_ids[0]], v_data[v_ids[1]], v_data[v_ids[2]]);
            const float cosa = test_normal.dotproduct(sml_normal);
            if (cosa < 0.034899f)   // cos(88°) — stop at hard creases
                continue;
            RecurseTri(adj, mView, W);
        }
    }
}

}  // namespace

void AddStatic(const char* texture, const Fvector& P, float size, CDB::TRI* pTri, const Fvector* pVerts)
{
    if (!texture || !texture[0] || !pTri || !pVerts || size < EPS_L) return;
    if (!g_pGameLevel) return;
    // R4 optimization cheat: no wallmarks farther than 100 m from the viewer.
    if (P.distance_to_sqr(Device.vCameraPosition) > _sqr(100.f)) return;

    // The mark is built into a local one under s_buildLock; s_lock only for the pool and the slot insert below.
    static_wallmark built;
    s_buildLock.Enter();

    // Query the level triangles around the hit, build adjacency (R4
    // add_static_wallmark_internal, processed inline instead of queued).
    Fbox bb_query;
    Fvector bbc, bbd;
    bb_query.set(P, P);
    bb_query.grow(size * 2.5f);
    bb_query.get_CD(bbc, bbd);

    xrXRC xrc;
    xrc.box_query(CDB::OPT_FULL_TEST, g_pGameLevel->ObjectSpace.GetStaticModel(), bbc, bbd);
    const u32 triCount = (u32)xrc.r_count();
    if (0 == triCount) { s_buildLock.Leave(); return; }

    CDB::TRI* tris = g_pGameLevel->ObjectSpace.GetStaticTris();
    sml_collector.clear();
    sml_collector.add_face_packed_D(pVerts[pTri->verts[0]], pVerts[pTri->verts[1]], pVerts[pTri->verts[2]], 0);
    for (u32 t = 0; t < triCount; t++)
    {
        CDB::TRI* T = tris + xrc.r_begin()[t].id;
        if (T == pTri)
            continue;
        sml_collector.add_face_packed_D(pVerts[T->verts[0]], pVerts[T->verts[1]], pVerts[T->verts[2]], 0);
    }
    sml_collector.calc_adjacency(sml_adjacency);

    Fvector N;
    N.mknormal(pVerts[pTri->verts[0]], pVerts[pTri->verts[1]], pVerts[pTri->verts[2]]);
    sml_normal.set(N);

    // 3D ortho-frustum (randomly rolled like R4's mRot)
    Fmatrix mView, mRot;
    BuildMatrix(mView, 1 / size, P);
    mRot.rotateZ(::Random.randF(deg2rad(-20.f), deg2rad(20.f)));
    mView.mulA_43(mRot);
    sml_clipper.CreateFromMatrix(mView, FRUSTUM_P_LRTB);

    RecurseTri(0, mView, built);

    if (built.verts.size() < 3)
    {
        s_buildLock.Leave();
        return;
    }

    // Push off the surface to kill z-fighting (see kNormalShift note above).
    for (auto& v : built.verts)
        v.p.mad(v.p, sml_normal, kNormalShift);
    s_buildLock.Leave();   // the scratch (sml_*) is free again; `built` is ours

    Fbox bb;
    bb.invalidate();
    for (const auto& v : built.verts)
        bb.modify(const_cast<Fvector&>(v.p));

    s_lock.Enter();
    static_wallmark* W = wm_allocate();
    W->verts.swap(built.verts);
    bb.getsphere(W->bounds.P, W->bounds.R);

    // Same-spot replace (don't stack identical decals), else register new.
    wm_slot* slot = FindOrAppendSlot(texture);
    for (auto& existing : slot->items)
    {
        if (existing->bounds.P.similar(W->bounds.P, 0.02f))
        {
            wm_destroy(existing);
            existing = W;
            s_lock.Leave();
            return;
        }
    }
    slot->items.push_back(W);

    s_lock.Leave();
}

void Render(FrameContext& ctx)
{
    const u32 skelCount = VK_SkelWM_Count();
    if (ctx.cmd == VK_NULL_HANDLE || !ctx.viewProj || !ParticlePass::Ready()
        || (s_slots.empty() && !skelCount)) {
        VK_SkelWM_EndFrame();   // drop this frame's queued skeleton marks regardless
        return;
    }

    VkPipeline pipe = ParticlePass::GetWallmarkPipeline();
    VkPipelineLayout layout = ParticlePass::GetLayout();
    if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) { VK_SkelWM_EndFrame(); return; }

    EnsureRings();   // (normally done by Prewarm at level load)
    CVulkanBuffer& ring = s_Ring[CommandManager.GetCurrentFrame()];
    if (!ring.IsValid()) { VK_SkelWM_EndFrame(); return; }
    u8* base = (u8*)ring.Map();
    if (!base) { VK_SkelWM_EndFrame(); return; }

    CFrustum view;
    Fmatrix vp = *ctx.viewProj;   // CreateFromMatrix takes a non-const ref
    view.CreateFromMatrix(vp, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
    // R4 ssaCLIP = r_ssaDISCARD/4 where r_ssaDISCARD = sqr(ps_r__ssaDISCARD) /
    // SCREEN AREA (r2_R_calculate.cpp:395) ≈ 3e-6 — NOT the raw console value
    // (3.5; using it raw culled every mark beyond ~10 cm: "дырок нет").
    const float screenArea = float(ctx.extent.width) * float(ctx.extent.height);
    const float ssaCLIP = (_sqr(ps_r__ssaDISCARD) / std::max(screenArea, 1.f)) / 4.f;

    // Collect this frame's vertices per slot (under the lock), then draw.
    struct DrawChunk { shared_str tex; u32 first; u32 count; };
    static xr_vector<DrawChunk> s_chunks;
    s_chunks.clear();
    u32 vtxUsed = 0;

    LoadProf::Push("wm.static");   // [rt prof] parts: Wallmarks 5-13 ms once or twice a firefight, unsplit (08-10)
    s_lock.Enter();
    for (wm_slot* slot : s_slots)
    {
        const u32 slotFirst = vtxUsed;
        for (auto it = slot->items.begin(); it != slot->items.end();)
        {
            static_wallmark* W = *it;
            if (view.testSphere_dirty(W->bounds.P, W->bounds.R))
            {
                const float dst = RV().vCameraPosition.distance_to_sqr(W->bounds.P);
                const float ssa = W->bounds.R * W->bounds.R / dst;
                if (ssa >= ssaCLIP && vtxUsed + W->verts.size() <= kRingVerts)
                {
                    // Fade with remaining ttl (R4 renders 1-ttl/TTL through an
                    // inverse-alpha blender — straight alpha here, same look).
                    float a = W->ttl / ps_r__WallmarkTTL;
                    int aC = iFloor(a * 255.f);
                    clamp(aC, 0, 255);
                    const u32 C = color_rgba(255, 255, 255, aC);
                    FVF::LIT* dst_v = (FVF::LIT*)(base + (size_t)vtxUsed * kVtxStride);
                    for (const auto& v : W->verts)
                    {
                        dst_v->p = v.p;
                        dst_v->color = C;
                        dst_v->t = v.t;
                        ++dst_v;
                    }
                    vtxUsed += (u32)W->verts.size();
                }
                W->ttl -= 0.1f * RV().fTimeDelta;   // visible marks fade much slower (R4)
            }
            else
                W->ttl -= RV().fTimeDelta;

            if (W->ttl <= EPS)
            {
                wm_destroy(W);
                it = slot->items.erase(it);
            }
            else
                ++it;
        }
        if (vtxUsed > slotFirst)
            s_chunks.push_back({ slot->texture, slotFirst, vtxUsed - slotFirst });
    }
    s_lock.Leave();

    // Skeleton wallmarks (blood on NPCs): CPU-skin each queued mark's faces
    // against the CURRENT pose into the ring (one chunk per mark — counts are
    // small and bounded by TTL). Already frustum-tested by CalculateWallmarks.
    LoadProf::Pop();
    LoadProf::Push("wm.skel");   // blood on NPCs: CPU-skinned against the current pose
    for (u32 i = 0; i < skelCount; ++i)
    {
        const u32 vc = VK_SkelWM_VCount(i);
        if (!vc || vtxUsed + vc > kRingVerts) continue;
        const char* tex = VK_SkelWM_Texture(i);
        if (!tex) continue;
        FVF::LIT* cursor = (FVF::LIT*)(base + (size_t)vtxUsed * kVtxStride);
        VK_SkelWM_Fill(i, cursor);
        s_chunks.push_back({ shared_str(tex), vtxUsed, vc });
        vtxUsed += vc;
        static bool s_diagSkel = false;
        if (!s_diagSkel) { s_diagSkel = true; Msg("[VK Wallmarks] first SKELETON mark drawn: tex='%s' verts=%u", tex, vc); }
    }
    VK_SkelWM_EndFrame();
    LoadProf::Pop();

    if (s_chunks.empty()) { ring.Unmap(); return; }

    // Render over the scene: colour LOAD + depth LOAD (test, no write) — the
    // same contract as the particle pass.
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

    // Generic decals = MODULATE2X (R4 parity); blood = its own alpha-blend
    // shader (readable red on dark clothing — modulate went near-black there).
    VkPipeline bloodPipe = ParticlePass::GetBloodWallmarkPipeline();
    VkPipeline lastPipe  = VK_NULL_HANDLE;
    u32 nDraw = 0;
    for (const DrawChunk& ch : s_chunks)
    {
        VkDescriptorSet set = ParticlePass::GetTextureSetAsync(ch.tex.c_str(), nullptr);   // a first use loads behind the frame
        if (set == VK_NULL_HANDLE) continue;   // missing decal texture
        VkPipeline want = strstr(ch.tex.c_str(), "blood") ? bloodPipe : pipe;
        if (want != lastPipe) { vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want); lastPipe = want; }
        vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(ctx.cmd, ch.count, 1, ch.first, 0);
        ++nDraw;
    }

    vkCmdEndRendering(ctx.cmd);
    ring.Flush();
    ring.Unmap();

    static bool s_diag = false;
    if (!s_diag && nDraw) { s_diag = true; Msg("[VK Wallmarks] first frame drawn: slots=%u verts=%u", nDraw, vtxUsed); }
}

// 📏08-10-2026 Switch v250/v258: [rt prof] "Wallmarks 9.4 ms" on the SAME frame (3197) of every firefight run = the first
// bullet hole: the decal + blood fragment shaders read from the SD and two graphics pipelines built on the render thread,
// plus the vertex rings. The rain pipelines are lazy the same way (the first rain). All of it here, behind the load screen.
void Prewarm()
{
    if (!ParticlePass::Ready()) return;
    const u64 t0 = CPU::QPC();
    ParticlePass::GetWallmarkPipeline();
    ParticlePass::GetBloodWallmarkPipeline();
    ParticlePass::GetRainPipeline();
    ParticlePass::GetRainDropPipeline();
    EnsureRings();
    Msg("[VK Wallmarks] prewarmed decal/blood/rain pipelines + rings in %.1f ms",
        1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq()));
}

void Clear()
{
    s_lock.Enter();
    for (wm_slot* slot : s_slots)
    {
        for (auto* W : slot->items)
            wm_destroy(W);
        xr_delete(slot);
    }
    s_slots.clear();
    s_lock.Leave();
}

void Destroy()
{
    Clear();
    s_lock.Enter();
    for (auto*& W : s_pool)
        xr_delete(W);
    s_pool.clear();
    if (s_RingInit)
    {
        for (u32 i = 0; i < kFramesInFlight; ++i)
            s_Ring[i].Destroy();
        s_RingInit = false;
    }
    s_lock.Leave();
}

}  // namespace Wallmarks
}  // namespace VK
