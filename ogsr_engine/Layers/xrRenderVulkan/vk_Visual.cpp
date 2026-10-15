// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_Visual.h"
#include "CRender_Vulkan.h"  // RImplementation (Shaders, model_*)
#include "../../xr_3da/device.h"   // load_screen_renderer -- the r_skin_lod_bg worker waits out a level load
#include "vk_buffer_pool.h"
#include "vk_shader.h"
#include "vk_material.h"
#include "vk_d3d_compat.h"   // VK_GetFVFVertexSize (FVF parsing)
#include "vk_UIPipeline.h"   // g_VkUI_FrameCmd
#include "vk_render_queue.h" // RenderQueue / DrawItem (phase-3 submission)
#include "vk_world_material.h"  // WorldMaterialCache (phase-5)
#include "../../xrCore/load_prof.h"   // per-spawn breakdown (geom/skin/model.material)
#include "vk_clk.h"          // rdtsc counters — CTimer truncates at this sample count
#include "../../../3rd_party/Src/meshoptimizer/src/meshoptimizer.h"  // ST skinned leaves: LOD windows at load

#include <string>
#include <atomic>
#include <unordered_map>     // fan clustering (vk_ComputeSynthBeams)
#include <algorithm>         // std::clamp/min/max (vk_BeamFromPoints)
#include <thread>            // boneFaces background builder
#include <condition_variable>
#include <functional>        // SkinWorker jobs
#include <chrono>            // the r_skin_lod_bg worker waits out a level load

// ============================================================================
// Visual-load profiling (Stage 0, pairs with load_steps in rvk_loader.cpp)
// ============================================================================
// Measured on pripyat_full: MT_NORMAL costs 76 us per visual (95248 of them =
// 7.2 s) while MT_TREE_ST costs 0.8 us - through the SAME three calls. So the
// cost is data-dependent, not path-dependent, and guessing which of the four
// parts owns it has already been wrong once. Split it and let the log say.
// ============================================================================
namespace VK { namespace VisualProf {

// In ticks, not milliseconds: each of these four fires once per visual, so at
// 450k visuals a truncating timer hides up to half a microsecond per sample --
// which is exactly how "header 0 ms" used to be printed for work that costs
// hundreds. See vk_clk.h.
u64   s_headerClk = 0, s_textureClk = 0, s_geomClk = 0, s_fastClk = 0;
u32   s_calls = 0;

// Inside LoadTexture, which owns most of the phase. Two parts of it run for
// every visual whether the visual has anything to say or not: the OGF texture
// chunk probe and the cache call that the real work hangs off.
u64   s_texOgfClk = 0, s_texCacheClk = 0;

void Dump()
{
    const float k = VK::ClkToMs();
    Msg("[load step]   visual Load split: header %.0f ms | texture %.0f ms | geometry %.0f ms | fastpath %.0f ms  (%u visuals)",
        k * float(s_headerClk), k * float(s_textureClk), k * float(s_geomClk), k * float(s_fastClk), s_calls);
    Msg("[load step]     LoadTexture split: ogf chunk %.0f | cache call %.0f | rest %.0f ms",
        k * float(s_texOgfClk), k * float(s_texCacheClk),
        k * float(s_textureClk - s_texOgfClk - s_texCacheClk));
    s_headerClk = s_textureClk = s_geomClk = s_fastClk = 0;
    s_texOgfClk = s_texCacheClk = 0;
    s_calls = 0;
}

}}

// ============================================================================
// VK_Render_Mesh implementation
// ============================================================================
VK_Render_Mesh::~VK_Render_Mesh()
{
    Destroy();
}

void VK_Render_Mesh::Destroy()
{
    // Only delete buffers if we own them (not a shared Copy)
    if (bOwnsBuffers)
    {
        if (p_rm_Vertices)
        {
            xr_delete(p_rm_Vertices);
        }

        if (p_rm_Indices)
        {
            xr_delete(p_rm_Indices);
        }

        if (m_fast)
        {
            xr_delete(m_fast);
        }
    }

    p_rm_Vertices = nullptr;
    p_rm_Indices = nullptr;
    m_fast = nullptr;
    vBase = vCount = vStride = 0;
    vAddr = 0;
    tcOffset = 24;
    iBase = iCount = 0;
    dwPrimitives = 0;
}

// ============================================================================
// vkRender_Visual implementation
// ============================================================================
vkRender_Visual::vkRender_Visual()
{
    Type = 0;
    skinning = -1;
}

vkRender_Visual::~vkRender_Visual() {}


void vkRender_Visual::Load(const char* name, IReader* data, u32 flags)
{
    dbg_name = name;

    u64 _c = CPU::GetCLK();

    // Load header (required)
    LoadHeader(data);
    { const u64 c = CPU::GetCLK(); VK::VisualProf::s_headerClk += c - _c; _c = c; }
    VK::VisualProf::s_calls++;

    // Load texture/shader info (optional)
    LoadTexture(data);
    VK::VisualProf::s_textureClk += CPU::GetCLK() - _c;
}

void vkRender_Visual::Release()
{
    // Base class has nothing to release
}

void vkRender_Visual::Copy(vkRender_Visual* from)
{
    Type = from->Type;
    vis = from->vis;
    skinning = from->skinning;
    dbg_name = from->dbg_name;
    m_pMaterial = from->m_pMaterial;
    m_fAlphaRef = from->m_fAlphaRef;
    m_bEmissiveAdd = from->m_bEmissiveAdd;
    // Pool instances lost the glass flag → the kinematics (skinned) path drew
    // furniture/door panes with the OPAQUE variant while the flag lived only on
    // the pool base. THE "cabinet glass ignores everything" bug (2026-07-02).
    m_bModelGlass = from->m_bModelGlass;
    m_bLitBlend   = from->m_bLitBlend;
    m_SynthBeams  = from->m_SynthBeams;   // pool clones must keep the synthesized beams
    // Pool instances are built via Copy(); without this the instance's diffuse
    // descriptor stays null and it renders white (the base had it from LoadTexture).
    // (Same latent class as the Update_Callback ctor bug.) WorldMaterial* is a shared
    // cache handle — safe to share.
    m_pWorldMaterial = from->m_pWorldMaterial;
}

void vkRender_Visual::Render(float LOD)
{
    // Base class does nothing
}

void vkRender_Visual::LoadHeader(IReader* data)
{
    // Read OGF header
    ogf_header H;
    if (data->find_chunk(OGF_HEADER))
    {
        data->r(&H, sizeof(H));

        Type = H.type;
        shader_id = H.shader_id;  // Phase 2.34: Store shader ID for rendering

        // Set visibility data (copy from OGF header structs to engine structs)
        vis.box.set(H.bb.min, H.bb.max);
        vis.sphere.set(H.bs.c, H.bs.r);
    }
    else
    {
        Msg("![Vulkan] Missing OGF_HEADER in %s", dbg_name.c_str());

        // Initialize to safe defaults
        Type = 0;
        shader_id = 0;  // Will use default shader (index 0) or fallback in Render()
        vis.box.set(Fvector().set(0,0,0), Fvector().set(0,0,0));
        vis.sphere.set(Fvector().set(0,0,0), 0.0f);
    }
}

void vkRender_Visual::LoadTexture(IReader* data)
{
    const u64 _cOgf0 = CPU::GetCLK();

    // Per-model diffuse from the OGF_TEXTURE chunk (dynamic models: NPCs, weapons,
    // items). Level statics have no OGF_TEXTURE — they resolve via shader_id below.
    string256 ogf_diffuse;
    ogf_diffuse[0] = 0;
    string256 ogf_shader;   // the terrain material keys off it when there is no level shader (below)
    ogf_shader[0] = 0;

    // Read texture chunk (optional - present in standalone OGF models)
    if (data->find_chunk(OGF_TEXTURE))
    {
        string256 texture_name;
        string256 shader_name;

        data->r_stringZ(texture_name, sizeof(texture_name));
        data->r_stringZ(shader_name, sizeof(shader_name));

        xr_strcpy(ogf_diffuse, texture_name);
        xr_strcpy(ogf_shader, shader_name);

        // Create material from texture name (loads DDS, creates descriptor set)
        if (g_MaterialManager && texture_name[0])
        {
            LoadProf::Scope _lp("model.material");
            m_pMaterial = g_MaterialManager->CreateMaterial(texture_name);
        }

        // Detect alpha-ref shader from OGF_TEXTURE shader name
        if (shader_name[0])
        {
            xr_string sn_lower = shader_name;
            std::transform(sn_lower.begin(), sn_lower.end(), sn_lower.begin(), ::tolower);
            // NOTE: this is a NAME heuristic, not the truth. The alpha reference really
            // lives in the blender definition in shaders.xr; matching substrings only
            // works for shaders whose name happens to say so. Anything alpha-tested whose
            // name does not contain one of these renders OPAQUE — foliage then shows the
            // black backing of its cards instead of being cut out.
            //
            // "leaf"        — flora\leaf_wave, the standard foliage blender (trunk_wave is
            //                 bark and correctly stays opaque, hence matching "leaf" and
            //                 not "flora").
            // "objects_lod" — def_shaders\def_objects_lod, the tree LOD billboards.
            // Both were missing and made every tree in a level render as black quads.
            if (sn_lower.find("aref") != xr_string::npos ||
                sn_lower.find("alpha") != xr_string::npos ||
                sn_lower.find("trans") != xr_string::npos ||
                sn_lower.find("leaf") != xr_string::npos ||
                sn_lower.find("objects_lod") != xr_string::npos)
            {
                m_fAlphaRef = 200.0f / 255.0f;  // DX11 def_aref uses oAREF=200
            }
            // Model glass panes: the OGF shaders "models\window" / *glass*
            // (= R4 CBlender_Model_EbB with blend: vehicle windows, furniture
            // panes, NPC glasses) or a glas\ texture — must BLEND with the R4
            // env-reflection formula; the opaque/alpha-test path made them
            // invisible or solid.
            xr_string tn_lower = texture_name;
            std::transform(tn_lower.begin(), tn_lower.end(), tn_lower.begin(), ::tolower);
            if (sn_lower.find("glass")  != xr_string::npos ||
                sn_lower.find("window") != xr_string::npos ||
                tn_lower.find("glas\\") != xr_string::npos ||
                tn_lower.rfind("glas", 0) == 0)
                m_bModelGlass = true;
            // Collimator / red-dot sight marks (R4 hud_reddotsight*.s: additive
            // blend(srcalpha, one), unlit). Through the regular lit skinned path
            // they rendered as a dark, barely visible smudge on the sight glass.
            // Fake light-beam planes (headlights/searchlights/weapon torches):
            // R4 renders these LIT + srcalpha-BLENDED (model_def_lq), NOT additive.
            if (sn_lower.find("lightplanes") != xr_string::npos)
            {
                m_bLitBlend = true;
                // Beam colour for the synthesized cones — the fan textures are
                // glow_<colour> (glow_orange headlights, glow_yellow torches…).
                Fvector& bc = m_SynthBeams.rgb;
                if      (tn_lower.find("orange") != xr_string::npos) bc.set(1.00f, 0.55f, 0.25f);
                else if (tn_lower.find("yellow") != xr_string::npos) bc.set(1.00f, 0.90f, 0.55f);
                else if (tn_lower.find("red")    != xr_string::npos) bc.set(1.00f, 0.30f, 0.15f);
                else if (tn_lower.find("blue")   != xr_string::npos) bc.set(0.45f, 0.70f, 1.00f);
                else if (tn_lower.find("green")  != xr_string::npos) bc.set(0.50f, 1.00f, 0.55f);
                else                                                 bc.set(0.88f, 0.94f, 1.00f);   // cool white (flashlight-like)
            }
            // `selflight_det*` is NOT one of these: despite the name its main pass is
            // a plain `deffer_model_flat` with bump+detail (see the stock .s script), so
            // it is an ordinary opaque material -- the first-person body uses it, and
            // shading it additively turned the player's legs into a ghost. Only the bare
            // `selflight`/`selflightl` lamp shaders want the additive treatment.
            const bool selflight_add = sn_lower.find("selflight") != xr_string::npos && sn_lower.find("_det") == xr_string::npos;

            if (sn_lower.find("reddot")      != xr_string::npos ||
                sn_lower.find("collimator")  != xr_string::npos ||
                sn_lower.find("holo")        != xr_string::npos ||
                selflight_add)                                     // lamp/projector self-lit faces
            {
                m_bEmissiveAdd = true;
                static int s_diag = 0;
                if (s_diag < 8) { ++s_diag; Msg("[VK Skinned] emissive-add leaf: shader='%s' tex='%s'", shader_name, texture_name); }
            }
        }
    }

    VK::VisualProf::s_texOgfClk += CPU::GetCLK() - _cOgf0;

    // Fallback: if no OGF_TEXTURE chunk (level geometry), use shader_id from header
    if (!m_pMaterial && RImplementation.Shaders.size() > 0)
    {
        if (shader_id < (u16)RImplementation.Shaders.size())
        {
            VK::CVulkanShader* pShader = RImplementation.Shaders[shader_id];
            if (pShader)
            {
                m_pMaterial = pShader->GetMaterial();
                if (pShader->m_bAlphaRef)
                    m_fAlphaRef = 200.0f / 255.0f;  // DX11 def_aref uses oAREF=200
            }
        }
    }

    // Phase 5: resolve the world-pass material from the same shader entry.
    // CMaterial above is parked-monolith infrastructure (g_MaterialManager is
    // a stub) — WorldMaterialCache is the live one we sample from. Lightmap
    // (3rd entry of the level shader's texture list) goes alongside diffuse
    // — null on vert-lit / unlit materials, which fall back to white.
    const char* diffuse_name = nullptr;
    const char* lmap_name    = nullptr;
    bool wmark = false;
    bool glass = m_bModelGlass;   // OGF "glass" shader (model/lamp panes)
    bool emis  = m_bEmissiveAdd;  // OGF selflight/reddot parts (world-path rigid models)
    bool water = false;           // level `effects\water*` bodies (Pass_Water)
    if (shader_id < (u16)RImplementation.Shaders.size()) {
        VK::CVulkanShader* pShader = RImplementation.Shaders[shader_id];
        if (pShader) {
            if (pShader->m_TexDiffuse.size() > 0) diffuse_name = pShader->m_TexDiffuse.c_str();
            if (pShader->m_TexLmap.size()    > 0) lmap_name    = pShader->m_TexLmap.c_str();
            wmark = pShader->m_bWmark;   // baked level decal (effects\wallmark*)
            if (!ogf_diffuse[0]) {
                glass |= pShader->m_bGlass;      // level windows (def_trans + glas\/wnd)
                emis  |= pShader->m_bEmissive;   // level glow billboards (effects\glow — lamp halos)
                water |= pShader->m_bWater;      // level water bodies (effects\water)
            }
        }
    }
    // Dynamic models (NPCs/weapons/items) carry their diffuse in OGF_TEXTURE, not in
    // the LEVEL shader table — their shader_id indexes that table arbitrarily. The
    // OGF's own texture is authoritative, so prefer it whenever present; level statics
    // (no OGF_TEXTURE) keep the shader-table diffuse.
    if (ogf_diffuse[0]) diffuse_name = ogf_diffuse;
    // GLASS renders through the wallmark pipeline variant: src-alpha blend, no
    // z-write, skips the depth prepass and the shadow-caster bins — exactly what a
    // translucent pane needs. The alpha-test is dropped (aref 0.78 discarded every
    // semi-transparent glass texel = the "no glass anywhere" bug); the texture's
    // alpha feeds the blend instead. aref = -2 is the GLASS marker the frags read:
    // they cap the blend alpha so a pane whose texture alpha saturates (glas_dirt
    // panes looked fully opaque in-game) still stays see-through.
    if (glass) { wmark = true; m_fAlphaRef = -2.0f; }
    // EMISSIVE-ADDITIVE (aref = -3): level `effects\glow` halos (the headlight's
    // "shine" — previously drawn OPAQUE = the broken orange disc) + selflight
    // parts. Additive pipeline + unlit FS, late flush like glass.
    if (emis)  { wmark = true; m_fAlphaRef = -3.0f; }
    // LIT-BLEND (aref = -4): lightplanes light beams — R4 model_def_lq verbatim
    // (lit colour, srcalpha blend, alpha = tex.a·fog²). Late flush like glass.
    if (m_bLitBlend) { wmark = true; m_fAlphaRef = -4.0f; }
    // WATER (aref = -5): level ponds/rivers. wmark keeps it out of the depth
    // prepass and the shadow-caster bins (a water plane must not occlude the
    // bottom it is supposed to show through, nor shadow it); the actual draw is
    // Pass_Water's, with its own pipeline — see vk_pass_water.cpp.
    if (water) { wmark = true; m_fAlphaRef = -5.0f; }

    // Level-shader name rides along for terrain materials: mask-less maps
    // regionalize terrain by SHADER (pripyat_asfalt/earth/grass), and the cache
    // synthesizes the one-hot splat mask from it (see GetOrCreate).
    const char* lvl_shader_name = nullptr;
    if (!ogf_diffuse[0] && shader_id < (u16)RImplementation.Shaders.size() && RImplementation.Shaders[shader_id])
        lvl_shader_name = RImplementation.Shaders[shader_id]->m_Name.c_str();
    // HOST-DRIVEN SCENES (the SDK editor): the guard above consults the level shader
    // table only when the OGF carries no diffuse of its own — but a host-exported
    // .ogf always does, so this stayed null and terrain lost its regionalization.
    // No shader name → no one-hot splat mask → the white fallback → all four detail
    // layers averaged at 0.25 each, which cancels their high-frequency content and
    // renders the ground as mush. The OGF's OWN shader name is right here and serves
    // exactly the same purpose, so fall back to it. Even when it matches no keyword
    // the heuristic still picks ONE channel — one detail at full strength beats a
    // four-way blur.
    if ((!lvl_shader_name || !lvl_shader_name[0]) && ogf_shader && ogf_shader[0])
        lvl_shader_name = ogf_shader;
    const u64 _cCache0 = CPU::GetCLK();
    m_pWorldMaterial = VK::WorldMaterialCache::GetOrCreate(diffuse_name, lmap_name, m_fAlphaRef, wmark, lvl_shader_name);
    VK::VisualProf::s_texCacheClk += CPU::GetCLK() - _cCache0;
}

// ============================================================================
// vkFVisual implementation - Standard triangle mesh
// ============================================================================
vkFVisual::vkFVisual()
{
    Type = MT_NORMAL;
}

vkFVisual::~vkFVisual()
{
    Release();
}

void vkFVisual::Load(const char* name, IReader* data, u32 flags)
{
    // Load base class data
    vkRender_Visual::Load(name, data, flags);

    u64 _c = CPU::GetCLK();

    // Load geometry (pass flags for VLOAD_NOVERTICES support)
    {
        LoadProf::Scope _lp("geom.buffers");
        LoadGeometry(data, flags);
    }
    { const u64 c = CPU::GetCLK(); VK::VisualProf::s_geomClk += c - _c; _c = c; }

    // Load fast-path (optional)
    {
        LoadProf::Scope _lp("geom.fastpath");
        LoadFastPath(data);
    }
    VK::VisualProf::s_fastClk += CPU::GetCLK() - _c;
}

void vkFVisual::Release()
{
    m_mesh.Destroy();
    vkRender_Visual::Release();
}

void vkFVisual::Copy(vkRender_Visual* from)
{
    vkRender_Visual::Copy(from);

    vkFVisual* src = dynamic_cast<vkFVisual*>(from);
    if (!src) return;

    // Share buffer references (not deep copy)
    // In X-Ray, meshes typically share buffers and only differ by shader
    // Mark as non-owning so Destroy() won't double-free the shared buffers
    m_mesh.p_rm_Vertices = src->m_mesh.p_rm_Vertices;
    m_mesh.vBase = src->m_mesh.vBase;
    m_mesh.vCount = src->m_mesh.vCount;
    m_mesh.vStride = src->m_mesh.vStride;
    m_mesh.tcOffset = src->m_mesh.tcOffset;
    m_mesh.vAddr = src->m_mesh.vAddr;   // same VkBuffer -> same device address

    m_mesh.p_rm_Indices = src->m_mesh.p_rm_Indices;
    m_mesh.iAddr = src->m_mesh.iAddr;   // same VkBuffer -> same device address
    m_mesh.iBase = src->m_mesh.iBase;
    m_mesh.iCount = src->m_mesh.iCount;

    m_mesh.bOwnsBuffers = false;  // Don't free shared buffers on destroy
    m_mesh.iType = src->m_mesh.iType;

    m_mesh.dwPrimitives = src->m_mesh.dwPrimitives;

    // Copy fast-path reference
    m_mesh.m_fast = src->m_mesh.m_fast;
}

void vkFVisual::Render(float /*LOD*/)
{
    // Phase 3: Render is no longer the world-pass entry point — Pass_World
    // calls Submit() instead, which queues a DrawItem; the queue's Flush()
    // does the actual binds + draw. Kept as an empty hook so direct callers
    // (HUD, particles) don't break, and to satisfy IRenderVisual.
}

void vkFVisual::Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD)
{
    if (!m_mesh.IsValid())                            return;
    if (!m_mesh.p_rm_Vertices || !m_mesh.p_rm_Indices) return;
    // Stride-12 / position-only layouts have no SHORT2 TC at tcOffset; pick
    // them up later with a position-only shader.
    if (m_mesh.vStride < m_mesh.tcOffset + 4)         return;

    VK::DrawItem it{};
    it.vis       = this;
    it.xform     = xform;
    it.lod       = LOD;
    it.lateGlass = m_pWorldMaterial && (m_pWorldMaterial->isGlass || m_pWorldMaterial->isEmisAdd || m_pWorldMaterial->isLitBlend);
    it.lateWater = m_pWorldMaterial && m_pWorldMaterial->isWater;
    it.sortKey = VK::makeSortKey(m_mesh.vStride, m_mesh.tcOffset,
                                 /*depthTest*/ true,
                                 m_pWorldMaterial,
                                 m_mesh.p_rm_Vertices,
                                 m_pWorldMaterial && m_pWorldMaterial->isWmark,
                                 m_pWorldMaterial && m_pWorldMaterial->tessellated);
    q.Push(it);
}


// ============================================================================
// ConvertFVFToLevelFormat - Convert FVF vertex data to stride-32 level format
// ============================================================================
// Level-static stride-32 layout (matches pipeline expectation):
//   FLOAT3  position   @ offset 0   (12 bytes)
//   D3DCOLOR normal    @ offset 12  (4 bytes)
//   D3DCOLOR tangent   @ offset 16  (4 bytes)  -- neutral 0x80808080
//   D3DCOLOR binormal  @ offset 20  (4 bytes)  -- neutral 0x80808080
//   SHORT2  tc0        @ offset 24  (4 bytes)  -- UV * 1024
//   SHORT2  tc1        @ offset 28  (4 bytes)  -- lightmap, zero
// Total: 32 bytes
//
static void ConvertFVFToLevelFormat(const u8* src, u8* dst, u32 vertCount, u32 fvf, u32 fvfStride)
{
    // Compute byte offsets of FVF components
    u32 posOffset = 0;
    u32 posSize = 0;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ:    posSize = 12; break;
    case D3DFVF_XYZRHW: posSize = 16; break;
    case D3DFVF_XYZB1:  posSize = 16; break;
    case D3DFVF_XYZB2:  posSize = 20; break;
    case D3DFVF_XYZB3:  posSize = 24; break;
    case D3DFVF_XYZB4:  posSize = 28; break;
    case D3DFVF_XYZB5:  posSize = 32; break;
    case D3DFVF_XYZW:   posSize = 16; break;
    default:            posSize = 12; break;
    }

    u32 off = posSize;
    u32 normalOffset = 0;
    bool hasNormal = false;
    if (fvf & D3DFVF_NORMAL)  { normalOffset = off; hasNormal = true; off += 12; }
    if (fvf & D3DFVF_PSIZE)   { off += 4; }
    if (fvf & D3DFVF_DIFFUSE) { off += 4; }
    if (fvf & D3DFVF_SPECULAR){ off += 4; }
    u32 texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    u32 tc0Offset = off;
    bool hasTC = (texCount > 0);

    const u32 LEVEL_STRIDE = 32;

    // X-Ray skinned/boned meshes carry a non-D3DFVF vertex code (e.g. 0x12071980,
    // stride 76) that we parse here as if it were a plain FVF. The derived
    // component offsets then point PAST the real vertex, so reading normal/uv
    // would over-read `src` and AV. Until the skinned-vertex path is ported
    // (vkSkeletonX_ST::_Load_hw), clamp every source read to within [0,fvfStride):
    // position is always the first 12 bytes (correct for boned verts too), and
    // normal/uv fall back to neutral when they don't fit. No crash; the mesh
    // loads static (no skinning) instead.
    const bool posOk    = (posOffset + 12 <= fvfStride);
    const bool normalOk = hasNormal && (normalOffset + 12 <= fvfStride);
    const bool tcOk     = hasTC && (tc0Offset + 8 <= fvfStride);

    for (u32 i = 0; i < vertCount; i++)
    {
        const u8* sv = src + i * fvfStride;
        u8* dv = dst + i * LEVEL_STRIDE;

        // Position: copy FLOAT3 (always first 12 bytes)
        if (posOk)
            CopyMemory(dv, sv + posOffset, 12);
        else
            ZeroMemory(dv, 12);

        // Normal: pack FLOAT3 -> D3DCOLOR
        if (normalOk) {
            const float* N = (const float*)(sv + normalOffset);
            u32 packed = color_rgba(vk_q_N(N[0]), vk_q_N(N[1]), vk_q_N(N[2]), 0);
            CopyMemory(dv + 12, &packed, 4);
        } else {
            u32 upNormal = color_rgba(vk_q_N(0.f), vk_q_N(1.f), vk_q_N(0.f), 0);
            CopyMemory(dv + 12, &upNormal, 4);
        }

        // Tangent + Binormal: neutral values
        u32 neutral = 0x80808080u;
        CopyMemory(dv + 16, &neutral, 4);
        CopyMemory(dv + 20, &neutral, 4);

        // TC0: convert FLOAT2 -> SHORT2 (multiply by 1024)
        if (tcOk) {
            const float* uv = (const float*)(sv + tc0Offset);
            s16 su = (s16)clampr(iFloor(uv[0] * 1024.f + 0.5f), -32768, 32767);
            s16 sv16 = (s16)clampr(iFloor(uv[1] * 1024.f + 0.5f), -32768, 32767);
            CopyMemory(dv + 24, &su, 2);
            CopyMemory(dv + 26, &sv16, 2);
        } else {
            ZeroMemory(dv + 24, 4);
        }

        // TC1 (lightmap): zero
        ZeroMemory(dv + 28, 4);
    }
}

// Fit ONE beam cone to a point cluster (model space). The fan is elongated
// along the beam (principal axis via power-iterated covariance) and WIDENS
// away from the lamp — the narrow end is the apex. See vkSynthBeam.
static bool vk_BeamFromPoints(const Fvector* pts, u32 count, vkSynthBeam& sb)
{
    if (!pts || count < 4) return false;

    Fvector c{ 0.f, 0.f, 0.f };
    for (u32 i = 0; i < count; ++i) c.add(pts[i]);
    c.div(float(count));

    float xx = 0, xy = 0, xz = 0, yy = 0, yz = 0, zz = 0;
    for (u32 i = 0; i < count; ++i) {
        Fvector d; d.sub(pts[i], c);
        xx += d.x * d.x; xy += d.x * d.y; xz += d.x * d.z;
        yy += d.y * d.y; yz += d.y * d.z; zz += d.z * d.z;
    }
    Fvector a = (xx >= yy && xx >= zz) ? Fvector{ 1, 0, 0 }
              : (yy >= zz)             ? Fvector{ 0, 1, 0 } : Fvector{ 0, 0, 1 };
    for (int it = 0; it < 16; ++it) {
        Fvector n{ xx * a.x + xy * a.y + xz * a.z,
                   xy * a.x + yy * a.y + yz * a.z,
                   xz * a.x + yz * a.y + zz * a.z };
        const float m = n.magnitude();
        if (m < 1e-9f) return false;
        n.div(m); a = n;
    }

    float tmin = 1e9f, tmax = -1e9f;
    for (u32 i = 0; i < count; ++i) {
        Fvector d; d.sub(pts[i], c);
        const float t = d.dotproduct(a);
        tmin = std::min(tmin, t); tmax = std::max(tmax, t);
    }
    const float span = tmax - tmin;
    if (span < 0.25f) return false;   // degenerate/tiny fan — not a beam

    // Radial spread in the 30% end slabs decides which end is the lamp.
    float rNear = 0, rFar = 0, rMax = 0; u32 nNear = 0, nFar = 0;
    for (u32 i = 0; i < count; ++i) {
        Fvector d; d.sub(pts[i], c);
        const float t = d.dotproduct(a);
        Fvector rad; rad.mad(d, a, -t);
        const float r = rad.magnitude();
        rMax = std::max(rMax, r);
        if      (t < tmin + 0.3f * span) { rNear += r; ++nNear; }
        else if (t > tmax - 0.3f * span) { rFar  += r; ++nFar;  }
    }
    if (!nNear || !nFar) return false;
    float nearAvg = rNear / float(nNear), farAvg = rFar / float(nFar);
    if (nearAvg > farAvg) {   // widens toward tmin → flip
        a.invert();
        const float t0 = tmin; tmin = -tmax; tmax = -t0;
        std::swap(nearAvg, farAvg);
    }
    // A real beam WIDENS away from the lamp. A symmetric spread (the zaz keeps
    // a single near-glow fan strung ACROSS the hood: its long axis runs between
    // the lamps, near/far spreads equal) is not a beam — reject it, the glow
    // billboard covers that look.
    if (farAvg < nearAvg * 1.6f + 0.02f) return false;

    sb.apex.mad(c, a, tmin);
    sb.dir  = a;
    sb.len  = span;
    sb.tanH = std::clamp(rMax / span, 0.08f, 1.2f);
    // Lamp-face radius: the fan's spread AT the lamp — the beam is a frustum
    // ("the whole headlight glows"), not a cone from a single point.
    sb.apexR = std::clamp(nearAvg * 1.2f, 0.05f, 0.6f);
    return true;
}

// Cluster a lightplanes leaf into connected fans, then fit a beam per fan.
// One leaf often holds SEVERAL fans (the zaz keeps BOTH headlights in one
// leaf: a merged PCA gave a cone in the middle of the hood pointing SIDEWAYS
// — the axis between the lamps, not along the beam). Components connect via
// shared triangle indices + a 1 cm positional weld (fans use unwelded quads).
static void vk_ComputeSynthBeams(const xr_vector<Fvector>& pos, const u16* idx, u32 idxCount,
                                 vkSynthBeams& out, const char* dbgName)
{
    out.count = 0;
    const u32 n = (u32)pos.size();
    if (n < 4) return;

    xr_vector<u32> parent(n);
    for (u32 i = 0; i < n; ++i) parent[i] = i;
    auto find = [&](u32 x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    auto unite = [&](u32 a, u32 b) { a = find(a); b = find(b); if (a != b) parent[b] = a; };

    if (idx && idxCount >= 3) {
        for (u32 t = 0; t + 2 < idxCount; t += 3) {
            const u32 a = idx[t], b = idx[t + 1], c = idx[t + 2];
            if (a >= n || b >= n || c >= n) continue;
            unite(a, b); unite(a, c);
        }
        // Positional weld: quads of one fan are often unwelded (same position,
        // separate vertices) — merge components sharing a 1 cm cell.
        std::unordered_map<u64, u32> cell;
        cell.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            const Fvector& p = pos[i];
            const u64 k = (u64(u32(s32(p.x * 100.f + 0.5f)) & 0x1FFFFF) << 42)
                        | (u64(u32(s32(p.y * 100.f + 0.5f)) & 0x1FFFFF) << 21)
                        |  u64(u32(s32(p.z * 100.f + 0.5f)) & 0x1FFFFF);
            auto it = cell.find(k);
            if (it == cell.end()) cell.emplace(k, i); else unite(it->second, i);
        }
    }
    // No indices available → single cluster (whole leaf), the old behaviour.

    std::unordered_map<u32, xr_vector<u32>> comps;
    for (u32 i = 0; i < n; ++i) comps[find(i)].push_back(i);
    xr_vector<const xr_vector<u32>*> order;
    for (auto& kv : comps)
        if (kv.second.size() >= 4) order.push_back(&kv.second);
    std::sort(order.begin(), order.end(),
              [](const xr_vector<u32>* a, const xr_vector<u32>* b) { return a->size() > b->size(); });

    xr_vector<Fvector> tmp;
    for (const xr_vector<u32>* comp : order) {
        if (out.count >= vkSynthBeams::kMax) break;
        tmp.clear(); tmp.reserve(comp->size());
        for (u32 vi : *comp) tmp.push_back(pos[vi]);
        vkSynthBeam sb{};
        if (!vk_BeamFromPoints(tmp.data(), (u32)tmp.size(), sb)) continue;
        out.b[out.count++] = sb;
    }

    // Merge near-duplicate beams: ONE lamp is often modelled as 2+ crossed
    // quads ~0.2-0.3 m apart (the zaz headlight came out as TWO parallel
    // beams) — same direction + close apexes collapse into one bright beam.
    // Distinct headlights (tr13: left/right pairs ~1 m apart) stay separate.
    for (u32 i = 0; i < out.count; ++i)
        for (u32 j = i + 1; j < out.count; ) {
            vkSynthBeam& A = out.b[i];
            const vkSynthBeam& B = out.b[j];
            // 0.6 m: crossed quads of ONE lamp sit 0.2-0.3 m apart, distinct
            // headlights ~1 m — a len-scaled radius merged the truck's pair.
            const float mergeR = 0.6f;
            if (A.dir.dotproduct(B.dir) > 0.9f && A.apex.distance_to(B.apex) < mergeR) {
                A.apex.add(B.apex); A.apex.mul(0.5f);
                A.dir.add(B.dir);   A.dir.normalize_safe();
                A.len   = std::max(A.len, B.len);
                A.tanH  = std::max(A.tanH, B.tanH);
                A.apexR = std::max(A.apexR, B.apexR);
                out.b[j] = out.b[--out.count];
            } else ++j;
        }

    for (u32 i = 0; i < out.count; ++i) {
        const vkSynthBeam& sb = out.b[i];
        Msg("[VK Cones] synth beam '%s' #%u: apex=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) len=%.2f tanH=%.2f (%u verts total)",
            dbgName ? dbgName : "?", i, sb.apex.x, sb.apex.y, sb.apex.z,
            sb.dir.x, sb.dir.y, sb.dir.z, sb.len, sb.tanH, n);
    }
}

void vkFVisual::LoadGeometry(IReader* data, u32 flags)
{
    BOOL bNoVertices = (flags & VLOAD_NOVERTICES) != 0;

    // Try OGF_GCONTAINER first (shared buffers - most common for level geometry)
    if (data->find_chunk(OGF_GCONTAINER))
    {
        // Container format:
        // u32 vb_id, u32 vb_offset, u32 vb_count
        // u32 ib_id, u32 ib_offset, u32 ib_count

        u32 vb_id = data->r_u32();
        u32 vb_offset = data->r_u32();
        u32 vb_count = data->r_u32();

        u32 ib_id = data->r_u32();
        u32 ib_offset = data->r_u32();
        u32 ib_count = data->r_u32();

        // OGF_GCONTAINER references shared VBs/IBs from CRender::nVB/nIB
        // (level.geom buffer pool). Visuals never own these — they're freed
        // once by `release_bufs(nVB,xVB,nIB,xIB)` in level_Unload. Without
        // this flag, ~vkFVisual would also try to free them → vkDestroyBuffer
        // on an invalid handle and an AV in level_Unload (caught by VK
        // validation as VUID-vkDestroyBuffer-buffer-parameter).
        m_mesh.bOwnsBuffers = false;

        // Always load index buffer info
        m_mesh.iBase = ib_offset;
        m_mesh.iCount = ib_count;
        m_mesh.dwPrimitives = ib_count / 3;

        // Get IB from pool
        if (VK::g_BufferPool)
        {
            m_mesh.p_rm_Indices = VK::g_BufferPool->GetIndexBuffer(ib_id);
            if (m_mesh.p_rm_Indices)
                m_mesh.iType = VK::g_BufferPool->GetIndexType(ib_id);
        }

        // Only load VB if not skipping vertices
        if (!bNoVertices)
        {
            m_mesh.vBase = vb_offset;
            m_mesh.vCount = vb_count;

            if (VK::g_BufferPool)
            {
                m_mesh.p_rm_Vertices = VK::g_BufferPool->GetVertexBuffer(vb_id);
                if (m_mesh.p_rm_Vertices)
                {
                    m_mesh.vStride = VK::g_BufferPool->GetVertexStride(vb_id);
                    m_mesh.tcOffset = VK::g_BufferPool->GetTexCoordOffset(vb_id);
                }
            }
        }

        if (!m_mesh.p_rm_Indices)
        {
            Msg("! [Vulkan] Visual '%s' geometry: IB[%u] not found in pool",
                dbg_name.c_str(), ib_id);
        }
        if (!bNoVertices && !m_mesh.p_rm_Vertices)
        {
            Msg("! [Vulkan] Visual '%s' geometry: VB[%u] not found in pool",
                dbg_name.c_str(), vb_id);
        }

        return;
    }

    // Lightplanes leaf: keep the positions until the indices are read below —
    // the beam synthesis clusters fans via the triangle list (vkSynthBeams).
    xr_vector<Fvector> lpPos;

    // Try inline vertices (OGF_VERTICES + OGF_INDICES) - standalone models (weapons, etc.)
    if (!bNoVertices && data->find_chunk(OGF_VERTICES))
    {
        u32 vert_format = data->r_u32();
        u32 vert_count = data->r_u32();

        // Compute FVF stride from format flags
        u32 fvf_stride = VK_GetFVFVertexSize(vert_format);
        if (fvf_stride == 0) fvf_stride = 32;

        Msg("[FVF] LoadGeometry '%s': FVF=0x%X stride=%u verts=%u",
            dbg_name.c_str(), vert_format, fvf_stride, vert_count);

        // A real static FVF must have a position component and <= 8 texcoord sets.
        // X-Ray skinned meshes store a non-FVF vertex declaration here (e.g.
        // 0x12071980 -> posMask=0, texCount=9) whose mis-derived stride (76) would
        // make the read below over-run the OGF chunk and AV. Until the skinned
        // vertex path (vkSkeletonX_ST::_Load_hw, from the monolith) is ported, skip
        // the inline VB for such meshes (they load index-only — invisible, no crash).
        const u32 posMask = vert_format & D3DFVF_POSITION_MASK;
        const u32 texCnt = (vert_format & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        const bool looksStaticFVF = (posMask != 0) && (texCnt <= 8);

        if (!looksStaticFVF)
        {
            Msg("! [VK] '%s': skinned/non-FVF verts (fmt=0x%X stride=%u) — skipping inline VB (skinned render not ported)",
                dbg_name.c_str(), vert_format, fvf_stride);
        }
        else
        {
            // Read raw FVF vertex data
            xr_vector<u8> raw_data(vert_count * fvf_stride);
            data->r(raw_data.data(), raw_data.size());

            // Convert FVF data to level-static stride-32 format
            u32 level_stride = 32;
            xr_vector<u8> converted(vert_count * level_stride);
            ConvertFVFToLevelFormat(raw_data.data(), converted.data(), vert_count, vert_format, fvf_stride);

            m_mesh.vStride = level_stride;
            m_mesh.vCount = vert_count;
            m_mesh.vBase = 0;

            // Lightplanes leaf (LoadTexture already set m_bLitBlend): stash the
            // positions for the beam synthesis after the indices are read.
            if (m_bLitBlend && !m_SynthBeams.count) {
                lpPos.reserve(vert_count);
                for (u32 i = 0; i < vert_count; ++i)
                    lpPos.push_back(*(const Fvector*)(converted.data() + size_t(i) * level_stride));
            }

            // Create and upload converted vertex buffer
            u32 data_size = vert_count * level_stride;
            m_mesh.p_rm_Vertices = xr_new<VK::CVulkanBuffer>();
            m_mesh.p_rm_Vertices->Create(
                data_size,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true
            );
            m_mesh.p_rm_Vertices->Upload(converted.data(), data_size);
        }
    }

    if (data->find_chunk(OGF_INDICES))
    {
        u32 idx_count = data->r_u32();
        m_mesh.iCount = idx_count;
        m_mesh.iBase = 0;
        m_mesh.dwPrimitives = idx_count / 3;
        m_mesh.iType = VK_INDEX_TYPE_UINT16;

        // Read index data; a subclass may append LOD windows after the OGF's own range (vkSkeletonX_ST).
        xr_vector<u16> idata(idx_count);
        data->r(idata.data(), idx_count * sizeof(u16));
        AppendIndexLods(idata);
        const u32 data_size = u32(idata.size() * sizeof(u16));

        // Create index buffer. DEVICE_ADDRESS + AS_BUILD_INPUT: the dynamic BLAS
        // (vk_rt_dyn) indexes the pre-skinned pool with THIS buffer, so a ray can
        // hit NPCs, doors and props — the same indices the raster draws with.
        m_mesh.p_rm_Indices = xr_new<VK::CVulkanBuffer>();
        m_mesh.p_rm_Indices->Create(
            data_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true
        );
        m_mesh.iAddr = m_mesh.p_rm_Indices->IsValid() ? m_mesh.p_rm_Indices->GetDeviceAddress() : 0;

        m_mesh.p_rm_Indices->Upload(idata.data(), data_size);

        if (!lpPos.empty())
            vk_ComputeSynthBeams(lpPos, idata.data(), idx_count, m_SynthBeams, dbg_name.c_str());
    }
}

void vkFVisual::LoadFastPath(IReader* data)
{
    // Fast-path is optional simplified geometry for shadow maps
    if (!data->find_chunk(OGF_FASTPATH))
        return;

    // TODO: Load fast-path geometry
    // Similar to LoadGeometry but into m_mesh.m_fast

}

// ============================================================================
// vkFProgressive implementation - LOD mesh with sliding-window index ranges
// ============================================================================
vkFProgressive::vkFProgressive()
{
    Type = MT_PROGRESSIVE;
}

vkFProgressive::~vkFProgressive()
{
    Release();
}

void vkFProgressive::Load(const char* name, IReader* data, u32 flags)
{
    // Base loads OGF_HEADER, OGF_TEXTURE, OGF_GCONTAINER (VB/IB), then we
    // append the LOD slide-window table.
    vkFVisual::Load(name, data, flags);
    LoadProf::Scope _lp("geom.swi");
    LoadSlidingWindow(data);
}

void vkFProgressive::Release()
{
    xr_free(sw_offsets);
    xr_free(sw_counts);
    sw_offsets = nullptr;
    sw_counts  = nullptr;
    sw_count   = 0;
    last_lod   = 0;
    vkFVisual::Release();
}

void vkFProgressive::Copy(vkRender_Visual* from)
{
    vkFVisual::Copy(from);

    vkFProgressive* src = dynamic_cast<vkFProgressive*>(from);
    if (!src) return;

    sw_count = src->sw_count;
    if (sw_count > 0)
    {
        sw_offsets = xr_alloc<u32>(sw_count);
        sw_counts  = xr_alloc<u32>(sw_count);
        CopyMemory(sw_offsets, src->sw_offsets, sw_count * sizeof(u32));
        CopyMemory(sw_counts,  src->sw_counts,  sw_count * sizeof(u32));
    }
    last_lod = 0;
}

void vkFProgressive::Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD)
{
    if (!m_mesh.IsValid())                            return;
    if (!m_mesh.p_rm_Vertices || !m_mesh.p_rm_Indices) return;
    if (m_mesh.vStride < m_mesh.tcOffset + 4)         return;
    if (sw_count == 0)                                return;

    // LOD index 0 = highest detail (full mesh). Real distance-based LOD pick
    // happens once we wire up per-visual world matrix + camera position;
    // sticking to highest detail keeps terrain visible at every range.
    const u32 lod_idx = 0;

    VK::DrawItem it{};
    it.vis            = this;
    it.xform          = xform;
    it.lod            = LOD;
    it.lateGlass      = m_pWorldMaterial && (m_pWorldMaterial->isGlass || m_pWorldMaterial->isEmisAdd || m_pWorldMaterial->isLitBlend);
    it.lateWater      = m_pWorldMaterial && m_pWorldMaterial->isWater;
    it.iBaseOverride  = m_mesh.iBase + sw_offsets[lod_idx];
    it.iCountOverride = sw_counts[lod_idx];
    it.sortKey        = VK::makeSortKey(m_mesh.vStride, m_mesh.tcOffset,
                                        /*depthTest*/ true,
                                        m_pWorldMaterial,
                                        m_mesh.p_rm_Vertices,
                                        m_pWorldMaterial && m_pWorldMaterial->isWmark,
                                        m_pWorldMaterial && m_pWorldMaterial->tessellated);
    q.Push(it);

    last_lod = lod_idx;
}

void vkFProgressive::LoadSlidingWindow(IReader* data)
{
    // OGF_SWIDATA layout (mirrors monolith / R4):
    //   u32 reserved[4]
    //   u32 count
    //   { u32 offset; u16 num_tris; u16 num_verts; } × count
    if (data->find_chunk(OGF_SWIDATA))
    {
        data->r_u32(); data->r_u32();
        data->r_u32(); data->r_u32();

        sw_count = data->r_u32();
        if (sw_count > 0)
        {
            sw_offsets = xr_alloc<u32>(sw_count);
            sw_counts  = xr_alloc<u32>(sw_count);
            for (u32 i = 0; i < sw_count; ++i)
            {
                sw_offsets[i] = data->r_u32();
                sw_counts[i]  = u32(data->r_u16()) * 3;  // num_tris → num_indices
                data->r_u16();                            // num_verts (unused)
            }
        }
    }

    // No SWI chunk (or zero-count) → degrade to single full-mesh "LOD".
    if (sw_count == 0)
    {
        sw_count   = 1;
        sw_offsets = xr_alloc<u32>(1);
        sw_counts  = xr_alloc<u32>(1);
        sw_offsets[0] = 0;
        sw_counts[0]  = m_mesh.iCount;
    }
}

// ============================================================================
// vkFHierrarhyVisual implementation - Composite visual
// ============================================================================
vkFHierrarhyVisual::vkFHierrarhyVisual()
{
    Type = MT_HIERRARHY;
    bDontDelete = FALSE;
}

vkFHierrarhyVisual::~vkFHierrarhyVisual()
{
    Release();
}

void vkFHierrarhyVisual::Load(const char* name, IReader* data, u32 flags)
{
    vkRender_Visual::Load(name, data, flags);

    // Try link-based children first (references to pre-loaded visuals)
    if (data->find_chunk(OGF_CHILDREN_L))
    {
        u32 count = data->r_u32();
        children.resize(count);

        for (u32 i = 0; i < count; ++i)
        {
            u32 id = data->r_u32();
            children[i] = static_cast<vkRender_Visual*>(RImplementation.getVisual(id));

            if (!children[i]) {
                Msg("![Vulkan] Hierarchy '%s': Failed to load child %u (visual ID %u)",
                    dbg_name.c_str(), i, id);
                Msg("![Vulkan] Child will be skipped during rendering");
            }
        }

        bDontDelete = TRUE;

        return;
    }

    // Try stream-based children (inline child definitions)
    if (data->find_chunk(OGF_CHILDREN))
    {
        IReader* chunk = data->open_chunk(OGF_CHILDREN);
        if (chunk)
        {
            u32 count = 0;
            IReader* child_chunk;

            while ((child_chunk = chunk->open_chunk(count)) != nullptr)
            {
                // Create child name: "parent_name:N"
                string_path child_name;
                xr_sprintf(child_name, "%s:%u", name, count + 1);

                // Create child visual
                IRenderVisual* child = RImplementation.model_CreateChild(child_name, child_chunk);
                if (child)
                {
                    children.push_back(static_cast<vkRender_Visual*>(child));
                }

                child_chunk->close();
                ++count;
            }

            chunk->close();
            bDontDelete = FALSE;

        }
    }
}

void vkFHierrarhyVisual::Release()
{
    if (!bDontDelete)
    {
        // Delete owned children
        for (auto& child : children)
        {
            if (child)
            {
                IRenderVisual* p = child;
                RImplementation.model_Delete(p, TRUE);
            }
        }
    }

    children.clear();

    vkRender_Visual::Release();
}

void vkFHierrarhyVisual::Copy(vkRender_Visual* from)
{
    vkRender_Visual::Copy(from);

    vkFHierrarhyVisual* src = dynamic_cast<vkFHierrarhyVisual*>(from);
    if (!src) return;

    // Deep copy children
    children.clear();
    children.reserve(src->children.size());

    for (u32 i = 0; i < src->children.size(); i++)
    {
        auto* src_child = src->children[i];
        IRenderVisual* child_copy = RImplementation.model_Duplicate(src_child);
        children.push_back(static_cast<vkRender_Visual*>(child_copy));
    }

    bDontDelete = FALSE;
}

void vkFHierrarhyVisual::Render(float LOD)
{
    // Phase 3: world-pass goes through Submit(); see vkFVisual::Render.
}

void vkFHierrarhyVisual::Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD)
{
    for (auto& child : children) {
        if (child) child->Submit(q, xform, LOD);
    }
}

void vkFHierrarhyVisual::Spawn()
{
    // Spawn all children
    for (auto& child : children)
    {
        if (child)
        {
            child->Spawn();
        }
    }
}

void vkFHierrarhyVisual::Depart()
{
    // Depart all children
    for (auto& child : children)
    {
        if (child)
        {
            child->Depart();
        }
    }
}

// ============================================================================
// vkFTreeVisual — base. Geometry loads through vkFVisual; we only add the
// OGF_TREEDEF2 chunk read (xform + per-instance lighting modulation).
// Render/Submit are no-ops: VK::CTreeManager extracts every tree at level
// load and draws all of them via GPU-driven indirect from a single SSBO.
// ============================================================================
vkFTreeVisual::vkFTreeVisual()
{
    Type = MT_TREE_ST;
    xform.identity();
    c_scale.rgb.set(1.f, 1.f, 1.f);
    c_scale.hemi = 1.f;
    c_scale.sun  = 1.f;
    c_bias.rgb.set(0.f, 0.f, 0.f);
    c_bias.hemi  = 0.f;
    c_bias.sun   = 0.f;
}

vkFTreeVisual::~vkFTreeVisual() {}

void vkFTreeVisual::Load(const char* name, IReader* data, u32 flags)
{
    vkFVisual::Load(name, data, flags);
    LoadTreeDef(data);

    // Trees always need alpha test for foliage cutout (matches DX11 oAREF=200)
    m_fAlphaRef = 200.f / 255.f;
}

void vkFTreeVisual::Render(float /*LOD*/) {}
void vkFTreeVisual::Submit(VK::RenderQueue& /*q*/, const Fmatrix& /*xform*/, float /*LOD*/) {}

void vkFTreeVisual::LoadTreeDef(IReader* data)
{
    if (!data->find_chunk(OGF_TREEDEF2))
    {
        Msg("! [vkFTreeVisual] OGF_TREEDEF2 missing in '%s'", dbg_name.c_str());
        return;
    }

    data->r(&xform, sizeof(xform));
    data->r(&c_scale, sizeof(c_scale));
    data->r(&c_bias,  sizeof(c_bias));

    // R4 lighting tweak: hemi halved and multiplied by ps_r__Tree_SBC * 4/3
    // (≈ 1.5 * 1.3333 = 2.0). Net effect on hemi is unchanged but kept verbatim
    // to match R4 numerics if SBC ever varies.
    constexpr float ps_r__Tree_SBC = 1.5f;
    constexpr float s = ps_r__Tree_SBC * 1.3333f;
    c_scale.hemi *= 0.5f * s;
    c_bias.hemi  *= 0.5f * s;
}

// ----------------------------------------------------------------------------
vkFTreeVisual_ST::vkFTreeVisual_ST()  { Type = MT_TREE_ST; }
vkFTreeVisual_ST::~vkFTreeVisual_ST() {}

// ----------------------------------------------------------------------------
vkFTreeVisual_PM::vkFTreeVisual_PM() { Type = MT_TREE_PM; }
vkFTreeVisual_PM::~vkFTreeVisual_PM() { Release(); }

void vkFTreeVisual_PM::Load(const char* name, IReader* data, u32 flags)
{
    vkFTreeVisual::Load(name, data, flags);
    LoadSlidingWindow(data);
}

void vkFTreeVisual_PM::Release()
{
    xr_free(sw_offsets);
    xr_free(sw_counts);
    sw_offsets = nullptr;
    sw_counts  = nullptr;
    sw_count   = 0;
    vkFTreeVisual::Release();
}

void vkFTreeVisual_PM::LoadSlidingWindow(IReader* data)
{
    // Trees store their sliding window POOLED via OGF_SWICONTAINER (an id into
    // CRender::SWIs), exactly like R4 FTreeVisual_PM — NOT the inline OGF_SWIDATA
    // that vkFProgressive uses. sw[0] is the max-quality LOD (R4 select_lod_id:
    // lod=1.0 → lod_id=0). TreeManager draws sw[0]'s range; without this the
    // full container index range includes collapsed PM data → stray "floating
    // branch" triangles.
    if (data->find_chunk(OGF_SWICONTAINER))
    {
        const u32 id = data->r_u32();
        FSlideWindowItem* it = RImplementation.getSWI((int)id);
        if (it && it->sw && it->count > 0)
        {
            sw_count   = it->count;
            sw_offsets = xr_alloc<u32>(sw_count);
            sw_counts  = xr_alloc<u32>(sw_count);
            for (u32 i = 0; i < sw_count; ++i)
            {
                sw_offsets[i] = it->sw[i].offset;
                sw_counts[i]  = (u32)it->sw[i].num_tris * 3;
            }
        }
        return;
    }

    // Fallback: inline OGF_SWIDATA (same layout as vkFProgressive).
    if (!data->find_chunk(OGF_SWIDATA)) return;

    data->r_u32(); data->r_u32();
    data->r_u32(); data->r_u32();

    sw_count = data->r_u32();
    if (sw_count == 0) return;

    sw_offsets = xr_alloc<u32>(sw_count);
    sw_counts  = xr_alloc<u32>(sw_count);
    for (u32 i = 0; i < sw_count; ++i)
    {
        sw_offsets[i] = data->r_u32();
        sw_counts[i]  = (u32)data->r_u16() * 3;  // num_tris * 3
        data->r_u16();                           // skip num_verts
    }
}

// ============================================================================
// vkFLOD — MT_LOD container with billboard imposter facets (OGF_LODDEF2).
// Inherits the hierarchy (children = full-detail meshes the tree manager already
// renders); adds the 8 pre-rendered billboard quads for the imposter pass.
// Mirrors R4 FLOD::Load (facet vertices are world-space; normal = averaged,
// inverted face normal used to pick the best facet per camera direction).
// ============================================================================
vkFLOD::vkFLOD()  { Type = MT_LOD; }
vkFLOD::~vkFLOD() {}

void vkFLOD::Load(const char* name, IReader* data, u32 flags)
{
    vkFHierrarhyVisual::Load(name, data, flags);   // header (vis.sphere) + children
    Type = MT_LOD;

    if (!data->find_chunk(OGF_LODDEF2))
        return;

    for (auto& facet : facets)
    {
        data->r(facet.v, sizeof(facet.v));         // 4 × LodVertex, raw

        Fvector N, T;
        N.set(0, 0, 0);
        T.mknormal(facet.v[0].v, facet.v[1].v, facet.v[2].v); N.add(T);
        T.mknormal(facet.v[1].v, facet.v[2].v, facet.v[3].v); N.add(T);
        T.mknormal(facet.v[2].v, facet.v[3].v, facet.v[0].v); N.add(T);
        T.mknormal(facet.v[3].v, facet.v[0].v, facet.v[1].v); N.add(T);
        N.div(4.f);
        facet.N.normalize(N);
        facet.N.invert();
    }
    facetsValid = true;
}

// ============================================================================
// MVP factory: only static meshes (MT_NORMAL) and composite hierarchies
// (MT_HIERRARHY) build real visuals. Everything else returns a dummy that
// reports its type but draws nothing — the loader needs SOMETHING valid for
// every Visuals[] slot, but an unsupported type silently no-ops at render
// time instead of crashing.
// ============================================================================
namespace {
class vkVisualDummy final : public vkRender_Visual
{
public:
    void Render(float /*LOD*/) override {}
};
} // namespace

// ============================================================================
// Skinned-mesh vertex load (STEP B sub-step 1) — ported from the monolith.
// vertBoned* = raw OGF boned vertex layouts; converted to vertHW_* (vk_Visual.h)
// and uploaded as a Vulkan VB. Bone deform / skinned draw is sub-step 2.
// ============================================================================
struct vertBoned1W { Fvector P; Fvector3 N; Fvector3 T; Fvector3 B; float u, v; u32 matrix; };
#pragma pack(push, 2)
struct vertBoned2W { u16 matrix0; u16 matrix1; Fvector P; Fvector N; Fvector T; Fvector B; float w; float u, v; };
struct vertBoned3W { u16 m[3]; Fvector P; Fvector N; Fvector T; Fvector B; float w[2]; float u, v; };
struct vertBoned4W { u16 m[4]; Fvector P; Fvector N; Fvector T; Fvector B; float w[3]; float u, v; };
#pragma pack(pop)

// Boned-vertex synth-beam variant: extract bind-pose MODEL-space positions
// (vertBoned*W stores P in model space — mRenderTransform includes the bind
// inverse) and run the same clustered fit as the FVF path. Skinned lightplanes
// carriers: physics vehicles' headlights (veh_zaz), searchlights, halogen lamps.
static void vk_ComputeSynthBeamsBoned(u32 dwVertType, const void* _verts_, u32 count,
                                      const u16* idx, u32 idxCount,
                                      vkSynthBeams& out, const char* dbgName)
{
    xr_vector<Fvector> pos;
    pos.reserve(count);
    switch (dwVertType) {
    case OGF_VERTEXFORMAT_FVF_1L: case 1: { auto* v = (const vertBoned1W*)_verts_; for (u32 i = 0; i < count; ++i) pos.push_back(v[i].P); } break;
    case OGF_VERTEXFORMAT_FVF_2L: case 2: { auto* v = (const vertBoned2W*)_verts_; for (u32 i = 0; i < count; ++i) pos.push_back(v[i].P); } break;
    case OGF_VERTEXFORMAT_FVF_3L: case 3: { auto* v = (const vertBoned3W*)_verts_; for (u32 i = 0; i < count; ++i) pos.push_back(v[i].P); } break;
    case OGF_VERTEXFORMAT_FVF_4L: case 4: { auto* v = (const vertBoned4W*)_verts_; for (u32 i = 0; i < count; ++i) pos.push_back(v[i].P); } break;
    default: return;
    }
    vk_ComputeSynthBeams(pos, idx, idxCount, out, dbgName);
}

bool vk_SkinNrmOldOrder()
{
    static const bool old = [] {
        const char* e = getenv("XROS_SKIN_NRM_OLD");
        const bool on = e && e[0] == '1';
        if (on)
            Msg("! [VK Skinned] XROS_SKIN_NRM_OLD=1: skinned normals packed in the OLD (z,y,x) order");
        return on;
    }();
    return old;
}

static void vkUploadConvertedVertices(VK_Render_Mesh& mesh, void* dst, u32 vStride, u32 vertCount)
{
    mesh.p_rm_Vertices = xr_new<VK::CVulkanBuffer>();
    // SHADER_DEVICE_ADDRESS: the compute pre-skinning pass (preskin.comp, see
    // Skinned_PreSkin) reads these vertices through a buffer reference instead
    // of a per-leaf descriptor.
    mesh.p_rm_Vertices->Create(vertCount * vStride,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
    mesh.p_rm_Vertices->Upload(dst, vertCount * vStride);
    mesh.vStride = vStride;
    mesh.vAddr   = mesh.p_rm_Vertices->GetDeviceAddress();
}

// Convert vertBoned* -> vertHW_* and upload. renderMode uses the vkSkeletonX_ST enum
// (identical in vkSkeletonX_PM). Bone index is stored RAW (0..BoneCount-1) — the
// skinned shader indexes the bones[] SSBO directly. (Earlier it used the legacy DX
// "*3" matrix-row stride, but that overflowed the u8 index channels in vertHW_* for
// bones >85 (idx*3>255), garbling whole regions — e.g. a sail-like spike at the
// shoulder. Direct indexing supports up to 255 bones, covering all real skeletons.)
static void vkLoadSkinnedVertices(VK_Render_Mesh& mesh, u16 renderMode, void* _verts_, u32 dwVertCount, const char* diagTag)
{
    using RM = vkSkeletonX_ST;
    switch (renderMode)
    {
    case RM::RM_SINGLE:
    case RM::RM_SKINNING_1B:
    {
        u32 vStride = sizeof(vertHW_1W);
        vertHW_1W* dst = xr_alloc<vertHW_1W>(dwVertCount);
        vertBoned1W* src = (vertBoned1W*)_verts_;
        for (u32 i = 0; i < dwVertCount; i++) {
            Fvector2 uv; uv.set(src->u, src->v);
            dst[i].set(src->P, src->N, src->T, src->B, uv, src->matrix);
            src++;
        }
        vkUploadConvertedVertices(mesh, dst, vStride, dwVertCount);
        xr_free(dst);
    } break;
    case RM::RM_SKINNING_2B:
    {
        u32 vStride = sizeof(vertHW_2W);
        vertHW_2W* dst = xr_alloc<vertHW_2W>(dwVertCount);
        vertBoned2W* src = (vertBoned2W*)_verts_;
        for (u32 i = 0; i < dwVertCount; i++) {
            Fvector2 uv; uv.set(src->u, src->v);
            dst[i].set(src->P, src->N, src->T, src->B, uv, src->matrix0, src->matrix1, src->w);
            src++;
        }
        vkUploadConvertedVertices(mesh, dst, vStride, dwVertCount);
        xr_free(dst);
    } break;
    case RM::RM_SKINNING_3B:
    {
        u32 vStride = sizeof(vertHW_3W);
        vertHW_3W* dst = xr_alloc<vertHW_3W>(dwVertCount);
        vertBoned3W* src = (vertBoned3W*)_verts_;
        for (u32 i = 0; i < dwVertCount; i++) {
            Fvector2 uv; uv.set(src->u, src->v);
            dst[i].set(src->P, src->N, src->T, src->B, uv, src->m[0], src->m[1], src->m[2], src->w[0], src->w[1]);
            src++;
        }
        vkUploadConvertedVertices(mesh, dst, vStride, dwVertCount);
        xr_free(dst);
    } break;
    case RM::RM_SKINNING_4B:
    {
        u32 vStride = sizeof(vertHW_4W);
        vertHW_4W* dst = xr_alloc<vertHW_4W>(dwVertCount);
        vertBoned4W* src = (vertBoned4W*)_verts_;
        for (u32 i = 0; i < dwVertCount; i++) {
            Fvector2 uv; uv.set(src->u, src->v);
            dst[i].set(src->P, src->N, src->T, src->B, uv, src->m[0], src->m[1], src->m[2], src->m[3], src->w[0], src->w[1], src->w[2]);
            src++;
        }
        vkUploadConvertedVertices(mesh, dst, vStride, dwVertCount);
        xr_free(dst);
    } break;
    default:
        Msg("! [%s] vkLoadSkinnedVertices: unknown RenderMode %u", diagTag ? diagTag : "SKL", (u32)renderMode);
        break;
    }
}

// Shared bone-analysis: scans the OGF boned vertices to pick RenderMode + bone count.
template <class TLeaf>
static void vk_skinned_analyse(TLeaf* leaf, u32 dwVertType, void* _verts_, u32 dwVertCount)
{
    xr_vector<u16> bids;
    u16 sw_bones_cnt = 0;
    switch (dwVertType)
    {
    case OGF_VERTEXFORMAT_FVF_1L:
    case 1: {
        vertBoned1W* pVO = (vertBoned1W*)_verts_;
        for (u32 it = 0; it < dwVertCount; ++it) {
            u16 mid = (u16)pVO[it].matrix;
            if (bids.end() == std::find(bids.begin(), bids.end(), mid)) bids.push_back(mid);
            sw_bones_cnt = _max(sw_bones_cnt, mid);
        }
        if (1 == bids.size()) { leaf->RenderMode = TLeaf::RM_SINGLE; leaf->RMS_boneid = *bids.begin(); }
        else { leaf->RenderMode = TLeaf::RM_SKINNING_1B; leaf->RMS_bonecount = sw_bones_cnt + 1; leaf->BonesUsed = (u16)bids.size(); }
    } break;
    case OGF_VERTEXFORMAT_FVF_2L:
    case 2: {
        vertBoned2W* pVO = (vertBoned2W*)_verts_;
        for (u32 it = 0; it < dwVertCount; ++it) {
            sw_bones_cnt = _max(sw_bones_cnt, pVO[it].matrix0);
            sw_bones_cnt = _max(sw_bones_cnt, pVO[it].matrix1);
            if (bids.end() == std::find(bids.begin(), bids.end(), pVO[it].matrix0)) bids.push_back(pVO[it].matrix0);
            if (bids.end() == std::find(bids.begin(), bids.end(), pVO[it].matrix1)) bids.push_back(pVO[it].matrix1);
        }
        leaf->RenderMode = TLeaf::RM_SKINNING_2B; leaf->RMS_bonecount = sw_bones_cnt + 1; leaf->BonesUsed = (u16)bids.size();
    } break;
    case OGF_VERTEXFORMAT_FVF_3L:
    case 3: {
        vertBoned3W* pVO = (vertBoned3W*)_verts_;
        for (u32 it = 0; it < dwVertCount; ++it)
            for (int k = 0; k < 3; k++) {
                sw_bones_cnt = _max(sw_bones_cnt, pVO[it].m[k]);
                if (bids.end() == std::find(bids.begin(), bids.end(), pVO[it].m[k])) bids.push_back(pVO[it].m[k]);
            }
        leaf->RenderMode = TLeaf::RM_SKINNING_3B; leaf->RMS_bonecount = sw_bones_cnt + 1; leaf->BonesUsed = (u16)bids.size();
    } break;
    case OGF_VERTEXFORMAT_FVF_4L:
    case 4: {
        vertBoned4W* pVO = (vertBoned4W*)_verts_;
        for (u32 it = 0; it < dwVertCount; ++it)
            for (int k = 0; k < 4; k++) {
                sw_bones_cnt = _max(sw_bones_cnt, pVO[it].m[k]);
                if (bids.end() == std::find(bids.begin(), bids.end(), pVO[it].m[k])) bids.push_back(pVO[it].m[k]);
            }
        leaf->RenderMode = TLeaf::RM_SKINNING_4B; leaf->RMS_bonecount = sw_bones_cnt + 1; leaf->BonesUsed = (u16)bids.size();
    } break;
    default:
        Msg("![Vulkan] vk_skinned_analyse: unknown vertex type 0x%X", dwVertType);
        leaf->RenderMode = TLeaf::RM_SINGLE; leaf->RMS_boneid = 0;
        break;
    }

    // DIAG (logs only on a new high-water mark): the largest bone index used by any
    // skinned leaf. A value > 85 confirms the legacy idx*3 u8 packing would have
    // overflowed (255/3) → the shoulder-spike artifact. Now stored RAW, safe to 255.
    static u16 s_maxBoneIdx = 0;
    if (sw_bones_cnt > s_maxBoneIdx) {
        s_maxBoneIdx = sw_bones_cnt;
        Msg("[VK Skinned] DIAG max bone index = %u (old idx*3 u8 overflow threshold = 85)", (u32)s_maxBoneIdx);
    }
}

// Retain a CPU copy of the boned vertices + indices and build per-bone face
// lists — skeleton wallmarks (blood on NPCs) and bone ray-picks read these
// (the GPU path only keeps the converted vertHW VB). Mirrors what R4 keeps in
// CSkeletonX (VerticesXW/m_Indices) + CBoneData face lists.
static std::shared_ptr<vkSkinCPUData> vk_skinned_retain_cpu(u32 dwVertType, void* _verts_, u32 dwVertCount, IReader* data)
{
    u32 links = 0, vsize = 0;
    switch (dwVertType) {
    case OGF_VERTEXFORMAT_FVF_1L: case 1: links = 1; vsize = sizeof(vertBoned1W); break;
    case OGF_VERTEXFORMAT_FVF_2L: case 2: links = 2; vsize = sizeof(vertBoned2W); break;
    case OGF_VERTEXFORMAT_FVF_3L: case 3: links = 3; vsize = sizeof(vertBoned3W); break;
    case OGF_VERTEXFORMAT_FVF_4L: case 4: links = 4; vsize = sizeof(vertBoned4W); break;
    default: return nullptr;
    }
    if (!data->find_chunk(OGF_INDICES)) return nullptr;
    const u32 iCount = data->r_u32();
    if (!iCount) return nullptr;
    // Bound the count against the bytes actually left in the chunk before slicing
    // data->pointer() — a truncated/corrupt OGF would otherwise read past the map.
    if ((size_t)iCount * sizeof(u16) > (size_t)data->elapsed()) return nullptr;

    auto d = std::make_shared<vkSkinCPUData>();
    d->links     = links;
    d->vertCount = dwVertCount;
    d->verts.assign((u8*)_verts_, (u8*)_verts_ + (size_t)vsize * dwVertCount);
    d->indices.assign((u16*)data->pointer(), (u16*)data->pointer() + iCount);

    return d;
}

// Built on first use -- see vkSkinCPUData::boneFaces in vk_Visual.h. Works from the
// retained copies (verts/indices), so the OGF reader is long closed by then.
const xr_vector<xr_vector<u32>>& vkSkinCPUData::boneFaces() const
{
    std::call_once(m_facesOnce, [this] {
        const void* _verts_ = verts.data();
        const u32 dwVertCount = vertCount;
        const u32 iCount = (u32)indices.size();
        const vkSkinCPUData* d = this;
        auto& outFaces = m_boneFaces;
        // Per-face bone set (deduped within the face) -> boneFaces[bone]. Two passes:
        // count first, then fill reserved lists instead of growing ~60 lists a face
        // at a time. Still 5-13 ms per leaf on the Switch, hence lazy.
        const u32 fCount = iCount / 3;
        auto faceBones = [&](u32 f, u16 (&fb)[12]) -> u32 {
            u32 nb = 0;
            for (u32 k = 0; k < 3; ++k) {
                const u32 vi = d->indices[f * 3 + k];
                if (vi >= dwVertCount) return 0;
                u16 b[4]; u32 cnt = 0;
                switch (links) {
                case 1: { auto& v = ((vertBoned1W*)_verts_)[vi]; b[0] = (u16)v.matrix; cnt = 1; } break;
                case 2: { auto& v = ((vertBoned2W*)_verts_)[vi]; b[0] = v.matrix0; b[1] = v.matrix1; cnt = 2; } break;
                case 3: { auto& v = ((vertBoned3W*)_verts_)[vi]; b[0] = v.m[0]; b[1] = v.m[1]; b[2] = v.m[2]; cnt = 3; } break;
                case 4: { auto& v = ((vertBoned4W*)_verts_)[vi]; b[0] = v.m[0]; b[1] = v.m[1]; b[2] = v.m[2]; b[3] = v.m[3]; cnt = 4; } break;
                }
                for (u32 j = 0; j < cnt; ++j) {
                    bool dup = false;
                    for (u32 e = 0; e < nb; ++e) if (fb[e] == b[j]) { dup = true; break; }
                    if (!dup && nb < 12) fb[nb++] = b[j];
                }
            }
            return nb;
        };
        xr_vector<u32> perBone;
        for (u32 f = 0; f < fCount; ++f) {
            u16 fb[12];
            const u32 nb = faceBones(f, fb);
            for (u32 e = 0; e < nb; ++e) {
                if (fb[e] >= perBone.size()) perBone.resize(fb[e] + 1, 0);
                ++perBone[fb[e]];
            }
        }
        outFaces.resize(perBone.size());
        for (size_t b = 0; b < perBone.size(); ++b)
            outFaces[b].reserve(perBone[b]);
        for (u32 f = 0; f < fCount; ++f) {
            u16 fb[12];
            const u32 nb = faceBones(f, fb);
            for (u32 e = 0; e < nb; ++e)
                outFaces[fb[e]].push_back(f);   // fb is deduped, so a face lands once per bone
        }
        m_facesReady.store(true, std::memory_order_release);
    });
    return m_boneFaces;
}

#ifdef __SWITCH__
extern "C" u32 svcSetThreadCoreMask(u32 thread, s32 preferred_core, u32 affinity_mask); // libnx
extern "C" u32 svcSetThreadPriority(u32 thread, u32 priority);                          // libnx
#endif

namespace
{
// A background worker for the skinned leaves' CPU work off the frame: one for the boneFaces tables, one for the
// r_skin_lod_bg windows. Each job holds a reference to what it builds, so a model released meanwhile is still built (and
// then freed) safely. Never joined: it only reads vertex copies it owns a share of, and sleeps when idle.
struct SkinWorker
{
    std::mutex mx;
    std::condition_variable cv;
    xr_deque<std::function<void()>> queue;
    bool started = false;
    u32 prio = 0;   // Switch: 0 = the default; else svcSetThreadPriority (a higher number = a lower priority)
    bool afterLoad = false;   // hold the jobs while the load screen is up

    void loop()
    {
#ifdef __SWITCH__
        // core 2: a new pthread otherwise lands on core 0, the main thread's
        svcSetThreadCoreMask(0xFFFF8000u /*CUR_THREAD_HANDLE*/, 2, 1u << 2);
        if (prio)
            svcSetThreadPriority(0xFFFF8000u, prio);
#endif
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(mx);
                cv.wait(l, [&] { return !queue.empty(); });
                job = std::move(queue.front());
                queue.pop_front();
            }
            while (afterLoad && load_screen_renderer.b_registered)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            job();
        }
    }
    void push(std::function<void()>&& job)
    {
        {
            std::lock_guard<std::mutex> g(mx);
            queue.push_back(std::move(job));
            if (!started)
            {
                started = true;
                std::thread([this] { loop(); }).detach();
            }
        }
        cv.notify_one();
    }
};

SkinWorker& skin_worker()
{
    static SkinWorker* w = new SkinWorker(); // leaked on purpose: no join at exit
    return *w;
}
// 09-10 v303: the windows of a level's models are seconds of work on core 2, and beside a level load they slowed it down:
// [npc prewarm] 683 -> 2565 ms, st_client_synchronising +2 s. Not a matter of the core: v304 at the lowest priority still
// 2162 ms (the reads on core 2 slow the main thread on core 0 too, see fs_load_prefetch). So they wait the load out, and
// the windows go live a moment after it; low priority still, under the prefetch reads of a spawn mid-game.
SkinWorker& skin_lod_worker()
{
    static SkinWorker* w = [] {
        SkinWorker* s = new SkinWorker();
        s->prio = 0x3B;
        s->afterLoad = true;
        return s;
    }();
    return *w;
}
} // namespace

void vkSkinCPUData::RequestFaces(const std::shared_ptr<vkSkinCPUData>& d)
{
    if (!d || d->facesReady() || d->m_facesQueued.exchange(true))
        return;
    skin_worker().push([d] { d->boneFaces(); });
}

extern float ps_r_skin_st_lod;
// Models whose skinned leaves get the r_skin_st_lod windows: NPCs, corpses and mutants. Leaf names are
// "<model path>:<n>" (vkFVisual::Load of the hierarchy), so the model's folder is at the front.
static bool vk_skin_lod_wanted(const char* name)
{
    if (!name)
        return false;
    while (*name == '\\' || *name == '/')
        ++name;
    string_path low;
    xr_strcpy(low, name);
    _strlwr(low);
    const bool want = !strstr(low, "hud") && (!strncmp(low, "actors", 6) || !strncmp(low, "monsters", 8));
    static int s_said[2] = {0, 0};   // the first few decisions, to see the filter read the names it was written for
    if (s_said[want] < 8) {
        ++s_said[want];
        Msg("[VK SkinLOD] %s '%s'", want ? "build" : "skip", name);
    }
    return want;
}

static std::shared_ptr<vkSkinLodWin> vk_skin_lod_request(const char* name, const std::shared_ptr<vkSkinCPUData>& cpu, u32 off0, u32 n,
                                                         const shared_str& dbg);   // below, with the r_skin_lod_bg builder

void vkSkeletonX_ST::Load(const char* name, IReader* data, u32 flags)
{
    R_ASSERT(data->find_chunk(OGF_VERTICES));
    u32 dwVertType  = data->r_u32();
    u32 dwVertCount = data->r_u32();
    void* _verts_   = data->pointer();

    {
        LoadProf::Scope _lp("skin.analyse");
        vk_skinned_analyse(this, dwVertType, _verts_, dwVertCount);
    }

    data->seek(0);
    // for AppendIndexLods, called from inside vkFVisual::Load -- characters only: weapons, artefacts and scopes paid
    // 15-38 ms each the first time one spawned mid-game (07-10 v187 [spawn prof] skin.st_lod) for windows that never
    // matter (a handful on screen, small, the HUD ones always at arm's length)
    m_lodVerts = (ps_r_skin_st_lod > 0.f && vk_skin_lod_wanted(name)) ? _verts_ : nullptr;
    m_lodVertType = dwVertType;
    m_lodVertCount = dwVertCount;
    vkFVisual::Load(name, data, flags | VLOAD_NOVERTICES);   // indices only (+ the LOD windows)
    m_lodVerts = nullptr;

    m_mesh.vBase = 0;
    m_mesh.vCount = dwVertCount;
    {
        LoadProf::Scope _lp("skin.vertices");
        _Load_hw_VK(_verts_, dwVertType, dwVertCount);
    }
    {
        LoadProf::Scope _lp("skin.cpu_copy");
        wmCPU = vk_skinned_retain_cpu(dwVertType, _verts_, dwVertCount, data);
    }
    // Lightplanes fan on a skinned leaf (headlights on physics vehicles,
    // searchlights, weapon torches) → synthesize the real beam cones (the
    // wallmark CPU copy supplies the triangle list for fan clustering).
    if (m_bLitBlend && !m_SynthBeams.count)
        vk_ComputeSynthBeamsBoned(dwVertType, _verts_, dwVertCount,
                                  wmCPU ? wmCPU->indices.data() : nullptr,
                                  wmCPU ? (u32)wmCPU->indices.size() : 0,
                                  m_SynthBeams, dbg_name.c_str());
    // no r_skin_st_lod windows (weapons, artefacts, devices; or r_skin_st_lod 0): the background-built ones (vkSkinLodWin)
    if (st_lod_count < 2)
        skinLod = vk_skin_lod_request(name, wmCPU, 0, m_mesh.iCount, dbg_name);
}
void vkSkeletonX_ST::_Load_hw_VK(void* verts, u32 /*vertType*/, u32 vertCount) { vkLoadSkinnedVertices(m_mesh, RenderMode, verts, vertCount, "SKL-ST"); }
void vkSkeletonX_ST::Copy(vkRender_Visual* from)
{
    vkFVisual::Copy(from);
    if (auto* s = dynamic_cast<vkSkeletonX_ST*>(from))
    {
        RenderMode = s->RenderMode; BonesUsed = s->BonesUsed; RMS_bonecount = s->RMS_bonecount; wmCPU = s->wmCPU;
        st_lod_count = s->st_lod_count;   // same index buffer (vkFVisual::Copy), same windows in it
        skinLod = s->skinLod;
        for (u32 i = 0; i < 4; ++i) { st_lod_off[i] = s->st_lod_off[i]; st_lod_cnt[i] = s->st_lod_cnt[i]; }
    }
}

// ⚠13-10 (the owner: "a black triangle sticks out of the TOZ-34 during the reload", "the NPCs shine like liquid
// terminators"): the windows were simplified on the bind-pose positions ALONE, with permissive collapses across every
// seam. So (1) a vertex of one bone's part could collapse into another bone's -- a cartridge's corner welded to the
// barrel, and when the reload moves the cartridge the triangle between them stretches out of the gun (the HUD draws
// window 1 at arm's length, r_skin_hud_lod); (2) a collapse across a normal seam left triangles joining vertices whose
// normals disagree, and the light smeared over the body like over liquid metal. Now every vertex on the border between
// two bones' parts is locked (a vertex's bone = its heaviest), and the normal is an attribute of the error metric.
namespace
{
struct SkinSimplifyIn
{
    xr_vector<Fvector> pos, nrm;
    xr_vector<u8> lock; // meshopt_SimplifyVertex_Lock (1): also what simplifySloppy reads as "do not move"
};
u16 skin_main_bone(const vertBoned1W& v) { return u16(v.matrix); }
u16 skin_main_bone(const vertBoned2W& v) { return v.w > .5f ? v.matrix1 : v.matrix0; } // P = lerp(P0, P1, w)
u16 skin_main_bone(const vertBoned3W& v)
{
    const float w[3] = {v.w[0], v.w[1], 1.f - v.w[0] - v.w[1]};
    return v.m[w[0] >= w[1] && w[0] >= w[2] ? 0 : w[1] >= w[2] ? 1 : 2];
}
u16 skin_main_bone(const vertBoned4W& v)
{
    const float w[4] = {v.w[0], v.w[1], v.w[2], 1.f - v.w[0] - v.w[1] - v.w[2]};
    int best = 0;
    for (int i = 1; i < 4; ++i)
        if (w[i] > w[best])
            best = i;
    return v.m[best];
}
// false: not a boned vertex layout. idx: the triangles the windows are made of (they decide the borders).
bool skin_simplify_in(SkinSimplifyIn& in, u32 links, const void* verts, u32 vc, const u32* idx, u32 n)
{
    xr_vector<u16> bone(vc);
    in.pos.resize(vc);
    in.nrm.resize(vc);
    auto take = [&](auto* v) {
        for (u32 i = 0; i < vc; ++i)
        {
            in.pos[i] = v[i].P;
            in.nrm[i] = v[i].N;
            bone[i] = skin_main_bone(v[i]);
        }
    };
    switch (links)
    {
    case 1: take((const vertBoned1W*)verts); break;
    case 2: take((const vertBoned2W*)verts); break;
    case 3: take((const vertBoned3W*)verts); break;
    case 4: take((const vertBoned4W*)verts); break;
    default: return false;
    }
    in.lock.assign(vc, 0);
    for (u32 t = 0; t + 2 < n; t += 3)
    {
        const u32 a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (bone[a] != bone[b] || bone[b] != bone[c])
            in.lock[a] = in.lock[b] = in.lock[c] = 1;
    }
    // a seam (another UV / normal at the same point) is one point to the simplifier: one lock for all its vertices
    struct PosKey
    {
        u32 x, y, z;
        bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct PosHash
    {
        size_t operator()(const PosKey& k) const { return size_t(k.x * 73856093u ^ k.y * 19349663u ^ k.z * 83492791u); }
    };
    auto key = [&](u32 i) {
        PosKey k;
        memcpy(&k.x, &in.pos[i].x, 4);
        memcpy(&k.y, &in.pos[i].y, 4);
        memcpy(&k.z, &in.pos[i].z, 4);
        return k;
    };
    xr_unordered_map<PosKey, u8, PosHash> at;
    at.reserve(vc);
    for (u32 i = 0; i < vc; ++i)
        at[key(i)] |= in.lock[i];
    for (u32 i = 0; i < vc; ++i)
        in.lock[i] = at[key(i)];
    return true;
}
constexpr float kSkinNrmWeight[3] = {.5f, .5f, .5f};
size_t skin_simplify(u32* dst, const u32* src, u32 n, const SkinSimplifyIn& in, size_t target, float error, float* err)
{
    return meshopt_simplifyWithAttributes(dst, src, n, &in.pos[0].x, in.pos.size(), sizeof(Fvector), &in.nrm[0].x, sizeof(Fvector), kSkinNrmWeight,
                                          3, in.lock.data(), target, error, meshopt_SimplifyLockBorder | meshopt_SimplifyPermissive, err);
}
} // namespace

// See vkSkeletonX_ST::st_lod_count. Simplified from the full index list each time (not from the previous window), on the
// bind-pose positions with the normals, the bones' borders locked (above) -- at the ranges these windows are drawn a
// joint's crease is a few pixels. Leaves under 300 triangles keep none (nothing to win); a window that saves under 20 % over the
// previous one ends the chain (the error limit stopped the simplifier).
extern float ps_r_skin_st_lod;
void vkSkeletonX_ST::AppendIndexLods(xr_vector<u16>& indices)
{
    st_lod_count = 1;
    const u32 n = (u32)indices.size();
    // 09-10 diag: since v188 no ST leaf gets windows (census "0 with built windows", v186 had 18/28) -- say why.
    static int s_said = 0;
    auto why = [&](const char* r, float a = 0.f, float b = 0.f) {
        if (m_lodVerts && s_said < 24) { ++s_said; Msg("[VK SkinLOD] '%s' %u tris: no windows -- %s (%.0f %.0f)", dbg_name.c_str(), n / 3, r, a, b); }
    };
    if (ps_r_skin_st_lod <= 0.f || !m_lodVerts || n < 3 * 300 || n % 3)
        return why(n < 3 * 300 ? "under 300 tris" : "cvar/odd count");
    LoadProf::Scope _lp("skin.st_lod");
    u32 links = 0;
    switch (m_lodVertType)
    {
    case OGF_VERTEXFORMAT_FVF_1L: case 1: links = 1; break;
    case OGF_VERTEXFORMAT_FVF_2L: case 2: links = 2; break;
    case OGF_VERTEXFORMAT_FVF_3L: case 3: links = 3; break;
    case OGF_VERTEXFORMAT_FVF_4L: case 4: links = 4; break;
    default: return why("vertex type", (float)m_lodVertType);
    }
    for (u16 i : indices)
        if (i >= m_lodVertCount)
            return why("index past the vertices", (float)i, (float)m_lodVertCount);   // a broken OGF: leave it alone

    const xr_vector<u32> src(indices.begin(), indices.end());
    SkinSimplifyIn in;
    skin_simplify_in(in, links, m_lodVerts, m_lodVertCount, src.data(), n);
    xr_vector<u32> dst(n);
    static constexpr float kRatio[3] = {.5f, .25f, .12f};
    static constexpr float kError[3] = {.02f, .05f, .10f};   // of the leaf's extent
    st_lod_off[0] = 0;
    st_lod_cnt[0] = n;
    u32 prev = n;
    for (u32 k = 0; k < 3; ++k)
    {
        const size_t target = size_t(float(n / 3) * kRatio[k]) * 3;
        float err = 0.f;
        // 09-10: an X-Ray skinned leaf splits ~3 vertices per position (normals/UV/bone weights: npc_provodnik 5012
        // verts on 1579 positions), so nearly every vertex is an attribute seam and the default simplifier, which only
        // collapses ALONG seams, stopped at once: 6200 -> 6190 tris at the 2 % error. That is why no ST leaf had a
        // window since v188. Permissive collapses across seams (a UV may stretch there -- these windows start at
        // r_skin_st_lod metres, on a 720p screen).
        const size_t got = skin_simplify(dst.data(), src.data(), n, in, target, kError[k], &err);
        if (!got || float(got) > .8f * float(prev)) {
            if (k == 0) why("simplifier kept > 80 %", float(got / 3), err * 1000.f);
            break;
        }
        st_lod_off[st_lod_count] = (u32)indices.size();
        st_lod_cnt[st_lod_count] = (u32)got;
        for (size_t i = 0; i < got; ++i)
            indices.push_back(u16(dst[i]));
        prev = (u32)got;
        ++st_lod_count;
    }
    if (st_lod_count > 1 && s_said < 24) {
        ++s_said;
        Msg("[VK SkinLOD] '%s' windows: %u -> %u / %u / %u tris", dbg_name.c_str(), n / 3, st_lod_cnt[1] / 3,
            st_lod_count > 2 ? st_lod_cnt[2] / 3 : 0u, st_lod_count > 3 ? st_lod_cnt[3] / 3 : 0u);
    }
}

// ---------------------------------------------------------------------------- r_skin_lod_bg: LOD windows built in the background
// See vkSkinLodWin (vk_Visual.h). Same simplifier, ratios and error limits as the ST windows above, from the full detail
// the skinned passes draw near (PM: OGF window 0), on the skin worker: a new model must not pay it in the spawn
// frame. Finished windows wait in s_lodDone until the extract side publishes them (vkSkinLodWin_Publish).
extern float ps_r_skin_lod_bg;
extern int   ps_r_skin_hud_lod;   // r_skin_hud_lod (vk_console_min.cpp): > 0 = the HUD models get windows too
namespace
{
std::mutex                                   s_lodDoneMx;
xr_vector<std::shared_ptr<vkSkinLodWin>>     s_lodDone;
std::atomic<u32>                             s_lodDoneN{0};
std::atomic<int>                             s_lodSaid{0};   // the first few leaves into the log

void vk_skin_lod_build(const std::shared_ptr<vkSkinLodWin>& w, const std::shared_ptr<vkSkinCPUData>& cpu, u32 off0, u32 n,
                     const xr_string& name)   // not a shared_str: its refcount is not atomic
{
    auto say = [&](const char* fmt, auto... a) {
        if (s_lodSaid.fetch_add(1) < 48) Msg(fmt, name.c_str(), n / 3, a...);
    };
    const u32 vc = cpu->vertCount;
    xr_vector<u32> src(n);
    for (u32 i = 0; i < n; ++i)
    {
        src[i] = cpu->indices[off0 + i];
        if (src[i] >= vc)
            return say("[VK SkinLOD] bg '%s' %u tris: no windows -- index %u past %u vertices", src[i], vc);
    }
    SkinSimplifyIn in;
    if (!skin_simplify_in(in, cpu->links, cpu->verts.data(), vc, src.data(), n))
        return say("[VK SkinLOD] bg '%s' %u tris: no windows -- %u links", cpu->links);
    CTimer t;
    t.Start();
    xr_vector<u32> dst(n);
    xr_vector<u16> out;
    // 10-10: a weapon goes two windows deeper (6%, 3%). The guns in NPC hands are the skinned triangles of a firefight
    // (user_fight3_v320: w.skinned 1.1 -> 3.25 units, a winchester1300 at 5 m 111k triangles in full, an ak101 ~45k), and
    // on the handheld screen a rifle 5 m away is ~100 px long, 15 m away a few dozen.
    static constexpr float kRatio[5] = {.5f, .25f, .12f, .06f, .03f};
    static constexpr float kError[5] = {.02f, .05f, .10f, .20f, .35f};   // of the leaf's extent
    const u32 levels = w->weapon ? 5u : 3u;
    w->off[0] = 0;
    w->cnt[0] = n;
    u32 prev = n, count = 1;
    for (u32 k = 0; k < levels; ++k)
    {
        const size_t target = size_t(float(n / 3) * kRatio[k]) * 3;
        float err = 0.f;
        // Permissive: an X-Ray skinned leaf is nearly all attribute seams (see vkSkeletonX_ST::AppendIndexLods)
        size_t got = skin_simplify(dst.data(), src.data(), n, in, target, kError[k], &err);
        // 10-10: a weapon's deep windows (12% and below) where the seams stop the simplifier -- an ak74 leaf ended at 12%, a
        // sig552 at 8%: the sloppy one, which welds across them. A gun 5+ m away is a few dozen pixels on the handheld screen
        // (the user: "I can hardly see it anyway").
        if (w->weapon && k >= 2 && (!got || float(got) > .8f * float(prev)))
            got = meshopt_simplifySloppy(dst.data(), src.data(), n, &in.pos[0].x, vc, sizeof(Fvector), in.lock.data(), target, 1.f, &err);
        if (!got || float(got) > .8f * float(prev))
        {
            if (k == 0)
                say("[VK SkinLOD] bg '%s' %u tris: no windows -- simplifier kept %u (err %.0f)", u32(got / 3), err * 1000.f);
            break;
        }
        w->off[count] = (u32)out.size();
        w->cnt[count] = (u32)got;
        for (size_t i = 0; i < got; ++i)
            out.push_back(u16(dst[i]));
        prev = (u32)got;
        ++count;
    }
    if (count < 2)
        return;
    // the index buffer is made by vkSkinLodWin_Publish: not on this low-priority thread, which must not hold the upload
    // lock the main thread flushes under while the prefetch thread runs ahead of it on core 2
    w->pending = std::move(out);
    w->count = count;
    say("[VK SkinLOD] bg '%s' windows: %u -> %u / %u / %u / %u / %u tris (%.1f ms)", w->cnt[1] / 3, count > 2 ? w->cnt[2] / 3 : 0u,
        count > 3 ? w->cnt[3] / 3 : 0u, count > 4 ? w->cnt[4] / 3 : 0u, count > 5 ? w->cnt[5] / 3 : 0u, t.GetElapsed_sec() * 1000.f);
    std::lock_guard<std::mutex> g(s_lodDoneMx);
    s_lodDone.push_back(w);
    s_lodDoneN.store((u32)s_lodDone.size(), std::memory_order_release);
}
} // namespace

// The r_skin_lod_bg request of a leaf (vkSkinLodWin): any non-HUD leaf, 300+ triangles in [off0, off0 + n) of the
// retained indices (fewer: nothing to win). The HUD models only while r_skin_hud_lod > 0 (else always at full detail).
static std::shared_ptr<vkSkinLodWin> vk_skin_lod_request(const char* name, const std::shared_ptr<vkSkinCPUData>& cpu, u32 off0, u32 n,
                                                         const shared_str& dbg)
{
    if (ps_r_skin_lod_bg <= 0.f || !cpu || n < 3 * 300 || n % 3 || size_t(off0) + n > cpu->indices.size() || !name)
        return nullptr;
    string_path low;
    xr_strcpy(low, name);
    _strlwr(low);
    // 10-10: a HUD model too while r_skin_hud_lod > 0 -- drawn at a fixed window of these (vk_pass_skinned SkinLeafDraw)
    const bool hud = strstr(low, "hud") != nullptr;
    if (hud && ps_r_skin_hud_lod <= 0)
        return nullptr;
    auto w = std::make_shared<vkSkinLodWin>();
    w->weapon = !hud && (strstr(low, "dynamics\\weapons\\") || strstr(low, "dynamics/weapons/"));   // before the worker reads it
    if (hud)
    {
        // held until drawn in first person (vkSkinLodWin_Kick, vk_pass_skinned SkinLeafDraw)
        w->dcpu = cpu;
        w->doff0 = off0;
        w->dn = n;
        w->dname = dbg.c_str();
        w->kicked.store(false, std::memory_order_release);
        return w;
    }
    skin_lod_worker().push([w, cpu, off0, n, nm = xr_string(dbg.c_str())] { vk_skin_lod_build(w, cpu, off0, n, nm); });
    return w;
}

void vkSkinLodWin_Kick(const std::shared_ptr<vkSkinLodWin>& w)
{
    if (!w || w->kicked.load(std::memory_order_acquire) || w->kicked.exchange(true))
        return;
    std::shared_ptr<vkSkinCPUData> cpu = std::move(w->dcpu);
    const u32 off0 = w->doff0, n = w->dn;
    xr_string nm = std::move(w->dname);
    skin_lod_worker().push([w, cpu, off0, n, nm] { vk_skin_lod_build(w, cpu, off0, n, nm); });
}

vkSkinLodWin::~vkSkinLodWin() { xr_delete(ib); }

// At most kPerFrame leaves a frame: a level's models finish by the hundred, and each is a buffer to create (a new NPC
// with a rifle is ~20 leaves).
void vkSkinLodWin_Publish(u32 frame)
{
    static constexpr size_t kPerFrame = 16;
    if (!s_lodDoneN.load(std::memory_order_acquire))
        return;
    xr_vector<std::shared_ptr<vkSkinLodWin>> batch;
    {
        std::lock_guard<std::mutex> g(s_lodDoneMx);
        const size_t n = _min(kPerFrame, s_lodDone.size());
        batch.assign(s_lodDone.begin(), s_lodDone.begin() + n);
        s_lodDone.erase(s_lodDone.begin(), s_lodDone.begin() + n);
        s_lodDoneN.store((u32)s_lodDone.size(), std::memory_order_release);
    }
    for (const auto& w : batch)
    {
        const u32 bytes = u32(w->pending.size() * sizeof(u16));
        VK::CVulkanBuffer* ib = xr_new<VK::CVulkanBuffer>();
        ib->Create(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, true);
        if (!ib->IsValid())
        {
            xr_delete(ib);
            Msg("! [VK SkinLOD] an index buffer of %u bytes failed -- that leaf keeps its own windows", bytes);
            continue;
        }
        // staged into the open upload batch, which this frame flushes before its submit waits on it
        // (CVulkanCommandManager::Submit); the windows are drawn from `frame`, a later one
        ib->Upload(w->pending.data(), bytes);
        xr_vector<u16>().swap(w->pending);
        w->ib = ib;
        w->liveFrame.store(frame, std::memory_order_release);
    }
}

void vkSkeletonX_PM::Load(const char* name, IReader* data, u32 flags)
{
    R_ASSERT(data->find_chunk(OGF_VERTICES));
    u32 dwVertType  = data->r_u32();
    u32 dwVertCount = data->r_u32();
    void* _verts_   = data->pointer();

    {
        LoadProf::Scope _lp("skin.analyse");
        vk_skinned_analyse(this, dwVertType, _verts_, dwVertCount);
    }

    data->seek(0);
    vkFProgressive::Load(name, data, flags | VLOAD_NOVERTICES);   // indices + sliding window only

    // SWI: pin the DRAW range to window 0 (max quality — R4 select_lod_id(1.0)).
    // The full index chunk also contains the coarser windows' replacement
    // triangles; the skinned pass draws m_mesh directly (no per-frame window
    // pick like vkFProgressive::Submit), so the full range stacked a coarse
    // copy of the mesh INSIDE the fine one — visible double-model on NPCs,
    // stretched "webbing" at extended joints, and wasted fragment work. Same
    // rule as vkFTreeVisual_PM (its stray "floating branch" fix). Trimming
    // here propagates to every consumer: color, prepass, shadows, VSM, MV.
    if (sw_count > 0)
    {
        m_mesh.iBase += sw_offsets[0];
        m_mesh.iCount = sw_counts[0];
    }

    m_mesh.vBase = 0;
    m_mesh.vCount = dwVertCount;
    {
        LoadProf::Scope _lp("skin.vertices");
        _Load_hw_VK(_verts_, dwVertType, dwVertCount);
    }
    // NOTE: wmCPU keeps the FULL index chunk (all LOD windows) — wallmark faces
    // may overlap across LODs; visually negligible (alpha decal).
    {
        LoadProf::Scope _lp("skin.cpu_copy");
        wmCPU = vk_skinned_retain_cpu(dwVertType, _verts_, dwVertCount, data);
    }
    if (m_bLitBlend && !m_SynthBeams.count)
        vk_ComputeSynthBeamsBoned(dwVertType, _verts_, dwVertCount,
                                  wmCPU ? wmCPU->indices.data() : nullptr,
                                  wmCPU ? (u32)wmCPU->indices.size() : 0,
                                  m_SynthBeams, dbg_name.c_str());
    // our LOD windows (vkSkinLodWin), from window 0
    if (sw_count)
        skinLod = vk_skin_lod_request(name, wmCPU, sw_offsets[0], sw_counts[0], dbg_name);
}
void vkSkeletonX_PM::_Load_hw_VK(void* verts, u32 /*vertType*/, u32 vertCount) { vkLoadSkinnedVertices(m_mesh, RenderMode, verts, vertCount, "SKL-PM"); }
void vkSkeletonX_PM::Copy(vkRender_Visual* from)
{
    vkFProgressive::Copy(from);
    if (auto* s = dynamic_cast<vkSkeletonX_PM*>(from)) { RenderMode = s->RenderMode; BonesUsed = s->BonesUsed; RMS_bonecount = s->RMS_bonecount; wmCPU = s->wmCPU; skinLod = s->skinLod; }
}

vkRender_Visual* vkVisual_CreateDummy()
{
    return xr_new<vkVisualDummy>();
}

// Real-skeleton factories live in the wrapper TUs: vkCreateKinematics() in
// vk_SkeletonCustom.cpp (CKinematics), vkCreateKinematicsAnimated() in
// vk_SkeletonAnimated.cpp (CKinematicsAnimated). These compile OGSR's own
// skeleton classes under the dxRender_Visual->vkRender_Visual mapping.
extern vkRender_Visual* vkCreateKinematics();
extern vkRender_Visual* vkCreateKinematicsAnimated();
extern vkRender_Visual* vkCreateKinematicsStub();

vkRender_Visual* vkVisual_Create(u32 type)
{
    switch (type)
    {
    case MT_NORMAL:
        return xr_new<vkFVisual>();

    case MT_HIERRARHY:
        return xr_new<vkFHierrarhyVisual>();

    case MT_PROGRESSIVE:
        // Level terrain / ground / any LOD-able static mesh. Geometry is
        // loaded the same way as MT_NORMAL; OGF_SWIDATA picks per-LOD IB
        // slices. Without this case, all progressive geometry routed to a
        // dummy and the world's ground was missing — visible as gaps where
        // you could see "under" the level.
        return xr_new<vkFProgressive>();

    case MT_TREE_ST:
        return xr_new<vkFTreeVisual_ST>();
    case MT_TREE_PM:
        return xr_new<vkFTreeVisual_PM>();

    case MT_LOD:
        // LOD container — OGF_CHILDREN_L (full-detail meshes, recursed by the
        // tree manager) PLUS OGF_LODDEF2 billboard facets (vkFLOD), drawn as
        // distant imposters by the LOD imposter manager.
        return xr_new<vkFLOD>();

    case MT_SKELETON_ANIM:
        // Real animated skeleton (CKinematicsAnimated). Children are the
        // vkSkeletonX_ST/_PM leaves below; CKinematics::Load child-handling is
        // patched (vk_SkeletonCustom_shared.inl) to link them.
        return vkCreateKinematicsAnimated();
    case MT_SKELETON_RIGID:
        // Real rigid skeleton (CKinematics).
        return vkCreateKinematics();

    // GeomDef leaves: placeholder vkSkeletonX_ST/PM (no rendering). Real
    // skinned mesh classes land in Session 2-3.
    case MT_SKELETON_GEOMDEF_PM:
        return xr_new<vkSkeletonX_PM>();
    case MT_SKELETON_GEOMDEF_ST:
        return xr_new<vkSkeletonX_ST>();

    default:
        // MT_PARTICLE_* / MT_LOD — valid OGF types, visual classes not yet
        // ported.
        Msg("[VK-Visual] type=%u not supported yet — using dummy", type);
        return vkVisual_CreateDummy();
    }
}
