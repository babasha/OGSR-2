// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

#include "stdafx.h"
#include "../../xr_3da/vis_common.h"
#include "../../Include/xrRender/RenderVisual.h"
#include "vk_buffer.h"

// Flag: skip vertex loading in base class (skinned meshes create their own VB)
#ifndef VLOAD_NOVERTICES
#define VLOAD_NOVERTICES (1<<0)
#endif

// ============================================================================
// Helper: quantize float normal component [-1,+1] to u8 [0,255]
// Matches DX11's q_N() in FSkinned.cpp
// ============================================================================
inline u8 vk_q_N(float v)
{
    int _v = clampr(iFloor((v + 1.f) * 127.5f), 0, 255);
    return u8(_v);
}

// Packed normal/tangent/binormal (xyz) + one byte (a), laid out in the MEMORY order the
// skinned shaders read: VK_FORMAT_R8G8B8A8_UNORM takes .r from the first byte. Not
// color_rgba -- that is D3DCOLOR (B,G,R,A in memory, DX swizzles it back on read), and
// through it every skinned normal reached the shaders as (z,y,x): the static path decodes
// the same D3DCOLOR with `.bgr`, the skinned one never did (04-10). The .a byte (weight or
// bone index) sits at byte 3 in both orders, which is why the geometry was never hurt.
// XROS_SKIN_NRM_OLD=1 restores the old (z,y,x) order at load -- A/B only.
bool vk_SkinNrmOldOrder();
inline u32 vk_pack_q(u8 x, u8 y, u8 z, u8 a)
{
    if (vk_SkinNrmOldOrder())
        std::swap(x, z);
    return u32(x) | (u32(y) << 8) | (u32(z) << 16) | (u32(a) << 24);
}

// ============================================================================
// Hardware vertex formats for skinned meshes
// These match DX11's vertHW_1W/2W/3W/4W from FSkinned.cpp
// vertBoned* (raw OGF data) is converted to these formats for GPU consumption
// ============================================================================

// 1-weight skinned vertex (36 bytes)
struct vertHW_1W
{
    float _P[4];    // Position (xyz) + 1.0 (w)
    u32   _N_I;     // Normal packed (RGB) + bone_index (A)
    u32   _T;       // Tangent packed (RGB)
    u32   _B;       // Binormal packed (RGB)
    float _tc[2];   // TexCoord UV

    void set(Fvector3& P, Fvector3 N, Fvector3 T, Fvector3 B, Fvector2& tc, int index)
    {
        N.normalize_safe();
        T.normalize_safe();
        B.normalize_safe();
        _P[0] = P.x; _P[1] = P.y; _P[2] = P.z; _P[3] = 1.f;
        _N_I = vk_pack_q(vk_q_N(N.x), vk_q_N(N.y), vk_q_N(N.z), u8(index));
        _T   = vk_pack_q(vk_q_N(T.x), vk_q_N(T.y), vk_q_N(T.z), 0);
        _B   = vk_pack_q(vk_q_N(B.x), vk_q_N(B.y), vk_q_N(B.z), 0);
        _tc[0] = tc.x; _tc[1] = tc.y;
    }
};  // sizeof = 36
static_assert(sizeof(vertHW_1W) == 36, "vertHW_1W must be 36 bytes");

// 2-weight skinned vertex (44 bytes)
struct vertHW_2W
{
    float _P[4];      // Position (xyz) + 1.0 (w)
    u32   _N_w;       // Normal packed (RGB) + weight0 (A)
    u32   _T;         // Tangent packed (RGB)
    u32   _B;         // Binormal packed (RGB)
    float _tc_i[4];   // tc.xy + bone_index0 (as float bits) + bone_index1 (as float bits)

    void set(Fvector3& P, Fvector3 N, Fvector3 T, Fvector3 B, Fvector2& tc,
             int index0, int index1, float w)
    {
        N.normalize_safe();
        T.normalize_safe();
        B.normalize_safe();
        _P[0] = P.x; _P[1] = P.y; _P[2] = P.z; _P[3] = 1.f;
        _N_w  = vk_pack_q(vk_q_N(N.x), vk_q_N(N.y), vk_q_N(N.z), u8(clampr(iFloor(w * 255.f + .5f), 0, 255)));
        _T    = vk_pack_q(vk_q_N(T.x), vk_q_N(T.y), vk_q_N(T.z), 0);
        _B    = vk_pack_q(vk_q_N(B.x), vk_q_N(B.y), vk_q_N(B.z), 0);
        _tc_i[0] = tc.x; _tc_i[1] = tc.y;
        // Store bone indices as actual float values for Vulkan
        // (DX11 used D3DDECLTYPE_SHORT2 which auto-converts s16->float,
        //  but Vulkan R32G32B32A32_SFLOAT reads raw float bits)
        _tc_i[2] = float(index0);
        _tc_i[3] = float(index1);
    }
};  // sizeof = 44
static_assert(sizeof(vertHW_2W) == 44, "vertHW_2W must be 44 bytes");

// 3-weight skinned vertex (44 bytes)
struct vertHW_3W
{
    float _P[4];      // Position (xyz) + 1.0 (w)
    u32   _N_w;       // Normal packed (RGB) + weight0 (A)
    u32   _T_w;       // Tangent packed (RGB) + weight1 (A)
    u32   _B_i;       // Binormal packed (RGB) + bone_index2 (A)
    float _tc_i[4];   // tc.xy + bone_index0 + bone_index1

    void set(Fvector3& P, Fvector3 N, Fvector3 T, Fvector3 B, Fvector2& tc,
             int index0, int index1, int index2, float w0, float w1)
    {
        N.normalize_safe();
        T.normalize_safe();
        B.normalize_safe();
        _P[0] = P.x; _P[1] = P.y; _P[2] = P.z; _P[3] = 1.f;
        _N_w  = vk_pack_q(vk_q_N(N.x), vk_q_N(N.y), vk_q_N(N.z), u8(clampr(iFloor(w0 * 255.f + .5f), 0, 255)));
        _T_w  = vk_pack_q(vk_q_N(T.x), vk_q_N(T.y), vk_q_N(T.z), u8(clampr(iFloor(w1 * 255.f + .5f), 0, 255)));
        _B_i  = vk_pack_q(vk_q_N(B.x), vk_q_N(B.y), vk_q_N(B.z), u8(index2));
        _tc_i[0] = tc.x; _tc_i[1] = tc.y;
        // Store bone indices as actual float values for Vulkan
        _tc_i[2] = float(index0);
        _tc_i[3] = float(index1);
    }
};  // sizeof = 44
static_assert(sizeof(vertHW_3W) == 44, "vertHW_3W must be 44 bytes");

// 4-weight skinned vertex (40 bytes)
struct vertHW_4W
{
    float _P[4];    // Position (xyz) + 1.0 (w)
    u32   _N_w;     // Normal packed (RGB) + weight0 (A)
    u32   _T_w;     // Tangent packed (RGB) + weight1 (A)
    u32   _B_w;     // Binormal packed (RGB) + weight2 (A)
    float _tc[2];   // TexCoord UV
    u32   _i;       // 4 bone indices packed as RGBA

    void set(Fvector3& P, Fvector3 N, Fvector3 T, Fvector3 B, Fvector2& tc,
             int index0, int index1, int index2, int index3,
             float w0, float w1, float w2)
    {
        N.normalize_safe();
        T.normalize_safe();
        B.normalize_safe();
        _P[0] = P.x; _P[1] = P.y; _P[2] = P.z; _P[3] = 1.f;
        _N_w = vk_pack_q(vk_q_N(N.x), vk_q_N(N.y), vk_q_N(N.z), u8(clampr(iFloor(w0 * 255.f + .5f), 0, 255)));
        _T_w = vk_pack_q(vk_q_N(T.x), vk_q_N(T.y), vk_q_N(T.z), u8(clampr(iFloor(w1 * 255.f + .5f), 0, 255)));
        _B_w = vk_pack_q(vk_q_N(B.x), vk_q_N(B.y), vk_q_N(B.z), u8(clampr(iFloor(w2 * 255.f + .5f), 0, 255)));
        _tc[0] = tc.x; _tc[1] = tc.y;
        // ⚠Byte ORDER, not color_rgba: that one is D3DCOLOR (0xAARRGGBB, i.e. B,G,R,A in
        // memory), which DX swizzles back on read, while VK_FORMAT_R8G8B8A8_UNORM takes the
        // bytes as they lie. color_rgba(i0,i1,i2,i3) therefore reached the shaders as
        // .r=i2 .b=i0 — every 4-weight vertex blended the wrong bone at weights w0/w2 and
        // shot off as a spike (the soldier/"Bulat" HUD hands, 04-10). The 1W/3W alpha
        // indices and the 2W/3W float indices never went through this swap.
        _i = u32(u8(index0)) | (u32(u8(index1)) << 8) | (u32(u8(index2)) << 16) | (u32(u8(index3)) << 24);
    }
};  // sizeof = 40
static_assert(sizeof(vertHW_4W) == 40, "vertHW_4W must be 40 bytes");

// Forward declarations
namespace VK {
    class CVulkanBuffer;
    class CMaterial;
    class RenderQueue;
    struct WorldMaterial;
}

// ============================================================================
// Synthesized light beams, derived from a `models\lightplanes` leaf's geometry
// at load (MODEL space). R4 bakes fake beam-plane fans into carrier models
// (car headlights, searchlights, halogen lamps) with NO dynamic light — we
// never draw the fans, so without synthesis the carriers were beam-less. Each fan is
// elongated along the beam and widens away from the lamp, so PCA recovers a
// real cone (apex/dir/len/half-angle) that Pass_LightCones raymarches like a
// genuine volumetric spot. A leaf often holds SEVERAL fans (both headlights in
// one leaf: a merged PCA pointed SIDEWAYS across the hood) — the vertices are
// clustered into connected components first and each fan gets its own beam.
// ============================================================================
struct vkSynthBeam
{
    Fvector apex{};                          // narrow end = the lamp (model space)
    Fvector dir{};                           // unit axis toward the wide end
    float   len   = 0.f;                     // beam length (m)
    float   tanH  = 0.f;                     // tan(half angle) at the wide end
    float   apexR = 0.08f;                   // lamp-face radius (fan width at the narrow end)
};

struct vkSynthBeams
{
    static constexpr u32 kMax = 4;
    vkSynthBeam b[kMax];
    u32     count = 0;
    Fvector rgb { 0.88f, 0.94f, 1.00f };     // from the glow texture name (LoadTexture);
                                             // default = cool white (flashlight-like)
};

// ============================================================================
// Vulkan Mesh Data - replaces IRender_Mesh from DX11
// Contains VkBuffer handles instead of D3D buffers
// ============================================================================
struct VK_Render_Mesh
{
    // Vertex buffer
    VK::CVulkanBuffer*  p_rm_Vertices = nullptr;
    u32                 vBase = 0;      // First vertex offset
    u32                 vCount = 0;     // Vertex count
    u32                 vStride = 0;    // Bytes per vertex
    u32                 tcOffset = 24;  // TEXCOORD0 byte offset (24=lmap, 28=vert-lit)

    // Device address of p_rm_Vertices, resolved ONCE at upload. Only skinned
    // leaves set it (vkUploadConvertedVertices) — the compute pre-skinning pass
    // reads the source vertices through it, which keeps that pass down to a
    // single descriptor set no matter how many leaves are on screen. Cached
    // here rather than in a VkBuffer->address map because VkBuffer handles are
    // recycled after a level unload and a stale map would hand out a dead
    // address. 0 = not addressable (level geometry, pooled buffers).
    VkDeviceAddress     vAddr = 0;

    // Index buffer
    VK::CVulkanBuffer*  p_rm_Indices = nullptr;
    u32                 iBase = 0;      // First index offset
    u32                 iCount = 0;     // Index count
    VkIndexType         iType = VK_INDEX_TYPE_UINT16;
    // Device address of p_rm_Indices — same rule as vAddr: set only for the
    // skinned leaves' own buffers (created with AS_BUILD_INPUT), 0 for pooled
    // level indices. The dynamic BLAS (vk_rt_dyn) needs both addresses.
    VkDeviceAddress     iAddr = 0;

    // Primitive info
    u32                 dwPrimitives = 0;

    // Fast-path geometry (for shadow maps)
    VK_Render_Mesh*     m_fast = nullptr;

    // Ownership flag: false when buffers are shared via Copy() (don't delete them)
    bool                bOwnsBuffers = true;

    VK_Render_Mesh() = default;
    ~VK_Render_Mesh();

    void Destroy();
    bool IsValid() const { return p_rm_Vertices != nullptr && vCount > 0; }
};

// ============================================================================
// vkRender_Visual - Base visual class for Vulkan renderer
// Equivalent to dxRender_Visual from DX11 renderer
// ============================================================================
class vkRender_Visual : public IRenderVisual
{
public:
    // Type from OGF header
    u32                 Type = 0;

    // Shader ID (index into RImplementation.Shaders array)
    // Phase 2.34: Shader-Material Binding
    u16                 shader_id = 0;

    // Visibility data (bounding box/sphere for culling)
    vis_data            vis;

    // Skinning quality (-1 = no skinning)
    s32                 skinning = -1;

    // Material (diffuse texture + descriptor set)
    VK::CMaterial*      m_pMaterial = nullptr;

    // Phase-5 world-pass material — resolved from shader_id during LoadTexture.
    // Always non-null after Load (defaults to white) so Submit doesn't have
    // to null-check.
    VK::WorldMaterial*  m_pWorldMaterial = nullptr;

    // Alpha-test threshold: -1.0 = disabled (solid), 0.5 = enabled (foliage/aref)
    float               m_fAlphaRef = -1.0f;

    // Additive unlit part (collimator/red-dot sight marks — OGF shader
    // hud\reddotsight*): rendered with (SRC_ALPHA, ONE) blend, no lighting,
    // no depth write, excluded from depth prepass + shadow casting.
    bool                m_bEmissiveAdd = false;

    // OGF shader "glass" (model/lamp panes): routed to the blended wallmark
    // pipeline variant in LoadTexture (see the GLASS note there).
    bool                m_bModelGlass = false;

    // OGF shader "lightplanes" (fake light-beam planes: headlights, searchlights,
    // weapon torches). R4 (CBlender_deffer_model, oBlend + aref<16 → forward
    // model_def_lq): LIT colour, srcalpha/invsrcalpha blend, alpha = tex.a·fog².
    bool                m_bLitBlend = false;

    // Cones synthesized from the lightplanes fan geometry (see vkSynthBeams) —
    // the fans are never drawn; these REAL volumetric beams replace them.
    vkSynthBeams        m_SynthBeams;

    // Debug name
    shared_str          dbg_name;

    // Debug ID (used by FHierrarhyVisual shared code)
    u32                 dbg_id = 0;

protected:
    // Render-flag — `setRZFlag/getRZFlag` from xrRender SkeletonCustom.h:76-77.
    // Default true, set to false to skip rendering this child visual.
    bool                _renderFlag = true;

public:
    inline bool         getRZFlag() const                { return _renderFlag; }
    inline void         setRZFlag(const bool f)          { _renderFlag = f; }

    vkRender_Visual();
    virtual ~vkRender_Visual();

    // Overload matching xrRender's dxRender_Visual::Render signature. The
    // Vulkan path doesn't use cmd_list / use_fast_geo (we have Submit instead),
    // so this just delegates to the LOD-only Render(). Skeleton wrappers call
    // this overload via the macro-mapped dxRender_Visual.
    virtual void Render(class CBackend& cmd_list, float lod, bool use_fast_geo) { Render(lod); }

    // ========================================================================
    // IRenderVisual interface
    // ========================================================================
    virtual vis_data&   _BCL getVisData() override { return vis; }
    virtual u32         getType() override { return Type; }
    virtual shared_str  getDebugName() const override { return dbg_name; }
    virtual shared_str  getDebugInfo() const override { return dbg_name; }

    // ========================================================================
    // Core methods - must be implemented by derived classes
    // ========================================================================

    // Load visual data from OGF file
    virtual void Load(const char* name, IReader* data, u32 flags);

    // Release GPU resources
    virtual void Release();

    // Copy from another visual (for duplication)
    virtual void Copy(vkRender_Visual* from);

    // Render the visual with specified LOD. After phase 3 this is a no-op
    // for the world pass — Pass_World traverses via Submit() instead.
    // Other paths (HUD, particles) may still call Render().
    virtual void Render(float LOD);

    // Phase-3 submission: push DrawItem(s) into the render queue. Pass_World
    // calls Submit on top-level Visuals; composite (Hier) visuals recurse
    // into children. Leaf vkFVisual::Submit pushes one DrawItem and does no
    // Vulkan work. Default (base class) is a no-op for skinned/particle
    // stubs and other inert types.
    virtual void Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD) {}

    // Called when instance is activated from pool
    virtual void Spawn() {}

    // Called when instance is returned to pool
    virtual void Depart() {}

    // Set debug ID (used by shared FHierrarhyVisual)
    virtual void setID(u32 id) { dbg_id = id; }

    // HeatVision support (stub)
    virtual void MarkAsHot(bool is_hot) {}

protected:
    // Load common data from OGF header
    void LoadHeader(IReader* data);

    // Load shader/texture info
    void LoadTexture(IReader* data);
};

// ============================================================================
// vkFVisual - Standard triangle mesh for Vulkan
// Equivalent to Fvisual from DX11 renderer
// ============================================================================
class vkFVisual : public vkRender_Visual
{
public:
    // Mesh data (vertex/index buffers)
    VK_Render_Mesh      m_mesh;

public:
    vkFVisual();
    virtual ~vkFVisual();

    // ========================================================================
    // vkRender_Visual overrides
    // ========================================================================
    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Release() override;
    virtual void Copy(vkRender_Visual* from) override;
    virtual void Render(float LOD) override;
    virtual void Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD) override;
    // LoadGeometry's own OGF_INDICES: a subclass may append more index ranges (LOD windows) after the OGF's, into the
    // same buffer, before it is created. m_mesh.iCount stays the OGF's count. (vkSkeletonX_ST, 07-10.)
    virtual void AppendIndexLods(xr_vector<u16>& /*indices*/) {}

protected:
    // Load geometry from OGF_GCONTAINER chunk
    // flags: 0 = normal, VLOAD_NOVERTICES = skip vertex loading (indices only)
    void LoadGeometry(IReader* data, u32 flags = 0);

    // Load fast-path geometry from OGF_FASTPATH chunk
    void LoadFastPath(IReader* data);
};

// ============================================================================
// vkFProgressive - LOD mesh with sliding-window index ranges. Mirrors OGSR
// R4's FProgressive (Layers/xrRender/FProgressive.h) and the monolith
// Vulkan port. Used for level terrain/ground and any other progressive-mesh
// static geometry. Geometry/material loading is identical to vkFVisual; the
// added OGF_SWIDATA chunk picks a (offset,count) IB slice per LOD level.
// ============================================================================
class vkFProgressive : public vkFVisual
{
public:
    u32  sw_count   = 0;       // Number of LOD levels in OGF_SWIDATA
    u32* sw_offsets = nullptr; // Per-LOD: index offset into m_mesh IB span
    u32* sw_counts  = nullptr; // Per-LOD: index count (num_tris * 3)
    u32  last_lod   = 0;

public:
    vkFProgressive();
    virtual ~vkFProgressive();

    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Release() override;
    virtual void Copy(vkRender_Visual* from) override;
    virtual void Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD) override;

protected:
    void LoadSlidingWindow(IReader* data);
};

// ============================================================================
// vkFTreeVisual - Tree visual (MT_TREE_ST / MT_TREE_PM). Extends vkFVisual
// for geometry loading. Render() / Submit() are no-ops — actual draw goes
// through VK::CTreeManager (extracted at level load, GPU-driven indirect).
// Stores xform + c_scale.hemi/c_bias.hemi from OGF_TREEDEF2.
// ============================================================================
class vkFTreeVisual : public vkFVisual
{
public:
    // OGF_TREEDEF2 chunk. xform is the per-instance world transform; the
    // hemi scale/bias modulate per-instance lighting (matched to R4 math:
    // halved + multiplied by ps_r__Tree_SBC*1.3333 ≈ 2.0).
    Fmatrix xform;
    struct TreeColor
    {
        Fvector3 rgb;       // Currently unused (R4 reads but does not feed)
        float    hemi;
        float    sun;       // Currently unused
    } c_scale, c_bias;

public:
    vkFTreeVisual();
    virtual ~vkFTreeVisual();

    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Render(float LOD) override;
    virtual void Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD) override;

protected:
    void LoadTreeDef(IReader* data);
};

class vkFTreeVisual_ST : public vkFTreeVisual
{
public:
    vkFTreeVisual_ST();
    virtual ~vkFTreeVisual_ST();
};

// Progressive tree — adds OGF_SWIDATA sliding window for LOD selection.
// TreeManager (Session C) reads sw_offsets/sw_counts to pick per-instance LOD.
class vkFTreeVisual_PM : public vkFTreeVisual
{
public:
    u32  sw_count   = 0;
    u32* sw_offsets = nullptr;  // index offset into IB span per LOD
    u32* sw_counts  = nullptr;  // index count (num_tris * 3) per LOD

    vkFTreeVisual_PM();
    virtual ~vkFTreeVisual_PM();

    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Release() override;

protected:
    void LoadSlidingWindow(IReader* data);
};

// ============================================================================
// CPU copy of a skinned leaf's boned vertices + indices + per-bone face lists.
// Needed for skeleton wallmarks (blood decals on NPCs) and bone ray-picking —
// the GPU-skinning path otherwise discards the vertex data after upload.
// Shared (shared_ptr) across model-pool copies of the same visual.
// ============================================================================
#include <memory>
#include <mutex>
#include <atomic>

struct vkSkinCPUData
{
    u32                       links = 0;       // bones per vertex (1..4) — picks the vertBoned*W layout
    u32                       vertCount = 0;
    xr_vector<u8>             verts;           // raw OGF vertBoned*W array
    xr_vector<u16>            indices;

    // bone id -> face indices (face = 3 consecutive indices). Built on FIRST USE (a
    // bullet's bone pick or a blood wallmark on this model), not at load: building
    // it walks the whole index chunk (all LOD windows) and cost 5-13 ms per leaf on
    // the Switch inside every first spawn of a model (v160 [spawn prof]
    // skin.bone_faces), for models most of which are never hit.
    const xr_vector<xr_vector<u32>>& boneFaces() const;

    // ⭐Blood decals must not build it in a frame: 📏07-10-2026 Switch v216 the
    // first wallmark on a new NPC model was 17-166 ms of the second thread
    // (CKinematics::AddWallmarkAsync, all leaves), the main thread waiting on it.
    // Ready = built; RequestFaces queues it for a background thread (core 2).
    bool facesReady() const { return m_facesReady.load(std::memory_order_acquire); }
    static void RequestFaces(const std::shared_ptr<vkSkinCPUData>& d);

private:
    mutable std::once_flag            m_facesOnce;
    mutable xr_vector<xr_vector<u32>> m_boneFaces;
    mutable std::atomic<bool>         m_facesReady{false};
    mutable std::atomic<bool>         m_facesQueued{false};
};

// ⭐LOD windows of our own for a non-HUD skinned leaf (09-10, the Switch, r_skin_lod_bg). The mod's sliding windows on
// NPC bodies barely thin anything out (23 NPCs at 60-120 m drew 368k of their 394k triangles), and the ST leaves of the
// weapons in NPC hands, artefacts and devices have none at all -- most of the skinned triangles of the Cordon's camps
// (v302). Built on the skin worker (core 2) from the full-detail range (PM: OGF window 0; ST: the whole index chunk) by
// meshoptimizer, ~50 / 25 / 12 % of its triangles, into an index buffer of their own; shared (shared_ptr) by every
// model-pool copy of the leaf. Window 0 stays the leaf's own m_mesh range; off/cnt[1..] are in `ib`.
// liveFrame: the frame (RV().frameIndex) from which the passes may draw them -- set on the extract side once per frame
// (vkSkinLodWin_Publish), so every pass of one frame, on whichever thread records it, picks the same windows.
struct vkSkinLodWin
{
    VK::CVulkanBuffer*    ib = nullptr;
    xr_vector<u16>        pending;             // windows 1.. from the worker, until vkSkinLodWin_Publish makes `ib` of them
    u32                   count = 1;           // windows, window 0 included
    bool                  weapon = false;      // 10-10: a dynamics/weapons model (NPC hands, the ground): 2 more windows (6%, 3%), r_skin_lod_wpn distances
    u32                   off[6]{};
    u32                   cnt[6]{};
    std::atomic<u32>      liveFrame{~0u};
    // 11-10: a HUD model's windows wait for its first first-person draw (vkSkinLodWin_Kick from SkinLeafDraw). The
    // stalkers' weapons load their HUD models too (player_hud prefetch, only for the animation lengths) and their
    // windows -- 10k-triangle leaves, ~100 ms each on core 2 -- were built for models the player may never hold.
    std::shared_ptr<vkSkinCPUData> dcpu;   // the held request
    u32                   doff0 = 0, dn = 0;
    xr_string             dname;
    std::atomic<bool>     kicked{true};   // false = held, the first Kick queues it
    ~vkSkinLodWin();
    bool live(u32 frame) const { return liveFrame.load(std::memory_order_acquire) <= frame; }
};
// Extract side, once per frame (Skinned_UploadBones): the windows the worker finished go live from frame `frame`.
void vkSkinLodWin_Publish(u32 frame);
// A held (HUD) request goes to the worker now; once. Any thread.
void vkSkinLodWin_Kick(const std::shared_ptr<vkSkinLodWin>& w);

// ============================================================================
// vkSkeletonX_ST / vkSkeletonX_PM — skinned-mesh leaves (children of CKinematics).
// Ported from the monolith. STEP B sub-step 1: Load reads the OGF boned vertices,
// analyses bones -> RenderMode, loads indices via the base Load(VLOAD_NOVERTICES),
// and converts vertBoned* -> vertHW_* into a Vulkan VB (_Load_hw_VK). Submit is a
// NO-OP for now: the vertHW VB can't be drawn by the static world pipeline (wrong
// vertex layout + bone-local positions) — the skinned pipeline/deform is sub-step 2.
// ============================================================================
class CKinematics;

class vkSkeletonX_ST : public vkFVisual
{
public:
    enum { RM_SKINNING_SOFT, RM_SINGLE, RM_SKINNING_1B, RM_SKINNING_2B, RM_SKINNING_3B, RM_SKINNING_4B };

    u16          RenderMode = RM_SINGLE;
    u16          BonesUsed  = 0;
    CKinematics* Parent     = nullptr;
    u16          child_idx  = 0;
    union { u32 RMS_boneid; u32 RMS_bonecount; };   // RM_SINGLE: bone id | skinning: max bone id+1

    // CPU skinning data for skeleton wallmarks / bone picks.
    std::shared_ptr<vkSkinCPUData> wmCPU;

    // ⭐LOD windows for a leaf that ships with none (07-10, the Switch). An ST leaf -- most of every NPC and corpse --
    // was drawn at full detail at any range: 200-320k skinned triangles in the Cordon's camps, x3 passes (prepass,
    // colour, cascade 0), ~5 of the ~11 profiler ms where the frame missed 60. Built at load by meshoptimizer (~50 /
    // 25 / 12 % of the triangles, borders locked so neighbouring leaves stay sealed) when r_skin_st_lod > 0, appended
    // to the leaf's own index buffer: window 0 = the OGF's indices at offset 0. Picked by distance in SkinLeafWindow.
    u32 st_lod_count = 1;
    u32 st_lod_off[4]{};
    u32 st_lod_cnt[4]{};
    // r_skin_lod_bg windows (vkSkinLodWin), for a leaf the r_skin_st_lod ones above skip: weapons, artefacts, devices.
    std::shared_ptr<vkSkinLodWin> skinLod;

    vkSkeletonX_ST() : RMS_bonecount(0) {}
    virtual void AppendIndexLods(xr_vector<u16>& indices) override;

    void AfterLoad(CKinematics* p, u16 idx) { Parent = p; child_idx = idx; }
    void SetParent(CKinematics* p)          { Parent = p; }

    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Copy(vkRender_Visual* from) override;
    // Sub-step 1 stopgap: skinned drawing not wired yet — don't push a vertHW VB
    // through the stride-32 world pipeline (garbage). Sub-step 2 replaces this.
    virtual void Submit(VK::RenderQueue& /*q*/, const Fmatrix& /*xform*/, float /*LOD*/) override {}

    void _Load_hw_VK(void* verts, u32 vertType, u32 vertCount);

private:
    // Load() hands its vertices to AppendIndexLods (called from inside vkFVisual::Load) through these; null outside Load.
    const void* m_lodVerts = nullptr;
    u32 m_lodVertType = 0, m_lodVertCount = 0;
};

class vkSkeletonX_PM : public vkFProgressive
{
public:
    enum { RM_SKINNING_SOFT, RM_SINGLE, RM_SKINNING_1B, RM_SKINNING_2B, RM_SKINNING_3B, RM_SKINNING_4B };

    u16          RenderMode = RM_SINGLE;
    u16          BonesUsed  = 0;
    CKinematics* Parent     = nullptr;
    u16          child_idx  = 0;
    union { u32 RMS_boneid; u32 RMS_bonecount; };

    // CPU skinning data for skeleton wallmarks / bone picks.
    std::shared_ptr<vkSkinCPUData> wmCPU;
    // Our LOD windows (r_skin_lod_bg > 0 at load); drawn instead of the OGF's sliding windows once live.
    std::shared_ptr<vkSkinLodWin> skinLod;

    vkSkeletonX_PM() : RMS_bonecount(0) {}

    void AfterLoad(CKinematics* p, u16 idx) { Parent = p; child_idx = idx; }
    void SetParent(CKinematics* p)          { Parent = p; }

    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Copy(vkRender_Visual* from) override;
    virtual void Submit(VK::RenderQueue& /*q*/, const Fmatrix& /*xform*/, float /*LOD*/) override {}

    void _Load_hw_VK(void* verts, u32 vertType, u32 vertCount);
};

// ============================================================================
// vkFHierrarhyVisual - Composite visual with children. Mirrors OGSR R4's
// FHierrarhyVisual (Layers/xrRender/FHierrarhyVisual.h): `children` is a
// public field, no getter. Hierarchy traversal walks the field directly.
// ============================================================================
class vkFHierrarhyVisual : public vkRender_Visual
{
public:
    xr_vector<vkRender_Visual*> children;
    BOOL                        bDontDelete = FALSE;

public:
    vkFHierrarhyVisual();
    virtual ~vkFHierrarhyVisual();

    // ========================================================================
    // vkRender_Visual overrides
    // ========================================================================
    virtual void Load(const char* name, IReader* data, u32 flags) override;
    virtual void Release() override;
    virtual void Copy(vkRender_Visual* from) override;
    virtual void Render(float LOD) override;
    virtual void Submit(VK::RenderQueue& q, const Fmatrix& xform, float LOD) override;
    virtual void Spawn() override;
    virtual void Depart() override;
};

// ============================================================================
// vkFLOD - LOD imposter container (MT_LOD). Extends the hierarchy (so the
// tree manager still recurses its full-detail children) and additionally loads
// the OGF_LODDEF2 billboard facets — 8 pre-rendered camera-direction quads into
// the level_lods atlas. The LOD imposter manager draws the best-facing facet of
// each FLOD for distant foliage density (mirrors R4 r_dsgraph_render_lods).
// ============================================================================
class vkFLOD : public vkFHierrarhyVisual
{
public:
    // Must match R4 FLOD::_vertex byte layout (read raw from OGF_LODDEF2).
    struct LodVertex
    {
        Fvector  v;            // world-space position
        Fvector2 t;            // UV into level_lods atlas
        u32      c_rgb_hemi;   // rgb + hemi
        u8       c_sun;        // sun factor
    };
    struct LodFacet
    {
        LodVertex v[4];        // quad corners
        Fvector   N;           // facet normal (computed at load)
    };

    LodFacet facets[8];
    bool     facetsValid = false;

    vkFLOD();
    virtual ~vkFLOD();
    virtual void Load(const char* name, IReader* data, u32 flags) override;
};

// ============================================================================
// Model type and OGF chunk IDs are defined in fmesh.h
// Include it for MT_*, OGF_* enums and ogf_header struct
// ============================================================================
#include "../../xr_3da/fmesh.h"

// ============================================================================
// MVP factories — only MT_NORMAL (vkFVisual) and MT_HIERRARHY (vkFHierrarhyVisual)
// build real visuals; everything else returns a non-rendering dummy so the
// loader survives unknown types instead of nullptr-dereffing.
// Skinned / progressive / particle / tree / LOD visual types from monolith
// are deferred until those subsystems are ported.
// ============================================================================
vkRender_Visual* vkVisual_Create(u32 type);
vkRender_Visual* vkVisual_CreateDummy();
