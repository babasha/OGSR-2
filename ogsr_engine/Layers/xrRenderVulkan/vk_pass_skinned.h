// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Skinned-mesh pass (STEP B sub-step 2).
//
// Draws the skinned leaves (vkSkeletonX_ST/_PM) of dynamic CKinematics visuals
// collected in g_DynamicVisuals. GPU skinning: per-skeleton bone matrices are
// written to a host-visible SSBO, the skinned pipeline (skinned.vert/frag.spv)
// blends 1-4 bones per vertex. Called inside Pass_World (shares color+depth).
#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"

class CFrustum;   // per-face cull for the point-cube skinned overlay

namespace VK
{
    void Pass_Skinned(FrameContext& ctx);
    void Skinned_Destroy();

    // Bone upload phase — computes bones for every dynamic/HUD skeleton this frame
    // and writes them into the SSBO PRE-MULTIPLIED by the object's world matrix
    // (so S*pos in the shaders is world-space). Idempotent per Device.dwFrame;
    // called by Pass_SunShadow (casters need bones before the world pass) and as a
    // fallback by Pass_Skinned.
    void Skinned_UploadBones();

    // COMPUTE PRE-SKINNING (r_preskin) — registered as the "PreSkin" pass, which
    // runs before every consumer. Uploads this frame's bones, then skins each
    // visible leaf ONCE into a shared world-space pool laid out as vertHW_1W.
    // The consumers are unchanged: they bind the pool and push skinMode=1 against
    // an identity bone, so their existing pipelines pass the pre-skinned position
    // and normal straight through. A leaf that doesn't fit the pool keeps the old
    // per-pass skinning, so the two paths mix safely within a frame.
    // MUST be recorded OUTSIDE a dynamic-rendering scope (it dispatches compute).
    //
    // ⭐РАЗДЕЛЕНО НА «РЕШИТЬ» И «ЗАПИСАТЬ» (19-09-2026). Prepare считает кости,
    // строит карту пула (s_psMap) и список диспатчей; PreSkin только проигрывает
    // список в командный буфер. Мотив: s_psMap читает КАЖДЫЙ рисующий скиннед-
    // пасс, и пока он строился по ходу записи, ни один из этих пассов нельзя было
    // отдать воркеру. Prepare зовётся из фазы prepare реестра (VK::RegisterPrepare)
    // на главном потоке до первой команды кадра; вызов из тела пасса оставлен и
    // идемпотентен, так что порядок без фазы prepare работает как прежде.
    void Skinned_PreSkinPrepare();
    void Skinned_PreSkin(VkCommandBuffer cmd);

    // DYNAMIC BLAS INPUTS (vk_rt_dyn, r_rt_dyn). One entry per world leaf that was
    // pre-skinned THIS frame: its world-space vertices sit in the pool at
    // poolFirst (stride 36, position first), its triangles are the leaf's own
    // index buffer at ibAddr. HUD and shadow-only skeletons are never listed.
    // Valid only for the current Device.dwFrame; empty otherwise.
    struct PreSkinRtLeaf { u32 poolFirst; u32 vertCount; VkDeviceAddress ibAddr; u32 indexCount; VkIndexType iType; };
    VkDeviceAddress                 Skinned_PreSkinPoolAddress();
    const xr_vector<PreSkinRtLeaf>& Skinned_PreSkinRtLeaves();

    // World-space foot-bone positions of this frame's skinned bodies (player + NPCs),
    // for snow footprint deformation. Valid after Skinned_UploadBones().
    void Skinned_CollectFeet(xr_vector<Fvector>& out, u32 maxFeet = 24);

    // DISCRETE footstep events: one entry per foot PLANT (heel-strike), not per frame —
    // gives separate boot prints instead of a dragged trench. dir = the body's facing
    // (world XZ) so the print can be boot-shaped/oriented. Valid after Skinned_UploadBones();
    // call ONCE per frame (it advances per-foot stance state).
    struct Footstep { Fvector pos; float dirX, dirZ; };
    void Skinned_CollectFootsteps(xr_vector<Footstep>& out, u32 maxSteps = 48);

    // World roots of skeleton-less skinned props/items (thrown/dropped bolts, grenades,
    // debris) — caller gates by ground proximity + prints them SHALLOW.
    void Skinned_CollectProps(xr_vector<Fvector>& out, u32 maxProps = 8);

    // Depth-only render of the world (non-HUD) skinned casters into the currently
    // bound shadow attachment. Caller owns render begin/end, viewport and bias.
    // Default cull = the sun ortho box; pass cullPos+cullRange to cull against a
    // light sphere instead (spot/point shadow maps). `frustum` (optional) adds a
    // per-view frustum test — a POINT cube passes each face's frustum so an NPC
    // renders only into the 1-2 faces it actually occupies, not all 6.
    void Skinned_RenderShadow(VkCommandBuffer cmd, const Fmatrix& lightVP,
                              const Fvector* cullPos = nullptr, float cullRange = 0.f,
                              const CFrustum* frustum = nullptr);

    // Any world skinned caster inside the sphere this frame? (cheap pre-check —
    // lets the point-cube shadow skip re-rendering when no one is near the light)
    bool Skinned_AnyCasterInSphere(const Fvector& pos, float range);

    // True when this visual is a skeleton that the skinned path draws. The rigid
    // sun-caster pass asks before drawing so the two never draw the same object — and it
    // asks here rather than repeating the test, so they cannot drift apart.
    // Takes IRenderVisual so this header stays free of the visual hierarchy.
    bool Skinned_HandlesVisual(IRenderVisual* v);

    // RENDER_THREAD_PLAN step 6b: the characters of the world dynamics list with the bounds their
    // bones produced this frame -- built by Skinned_UploadBones (extract side), read by the capsule
    // shadows pass so it no longer touches CKinematics / vis.box at record time.
    struct CharacterBounds { IRenderVisual* vis; Fmatrix xform; Fbox box; bool noShadow; };
    const xr_vector<CharacterBounds>& Skinned_CharacterBounds();

    // NPCs into the scene depth PREPASS (camera VP) — GTAO occluders + early-Z.
    // Alpha-tested with skinned.frag's threshold so cutout coverage matches the
    // color pass. HUD skeletons never enter. Called from Pass_World's prepass.
    // mvTarget: the prepass scope carries the RG16F motion attachment (r_mv_prepass),
    // so bind the twin that declares it (mask 0 — NPC motion still arrives from the
    // overlay in MotionVecDyn).
    void Skinned_RenderDepthPrepass(VkCommandBuffer cmd, const Fmatrix& viewProj, bool mvTarget = false);
    // r_hud_prepass: the first-person list into the prepass depth at the HUD near range (no motion attachment).
    void Skinned_RenderHudDepthPrepass(VkCommandBuffer cmd, const VkExtent2D& ext);

    // NPCs into the SSAO normal G-buffer (camera VP) — feeds GTAO real per-pixel
    // normals where an NPC is visible (kills depth-derivative speckle on NPCs).
    // Depth-tested against the prepass depth (no depth write); alpha-tested like
    // the depth prepass. Caller owns render begin/end + viewport (Pass_World).
    void Skinned_RenderNormalPrepass(VkCommandBuffer cmd, const Fmatrix& viewProj);

    // NPC motion vectors (MV Phase 2a): re-draw world skinned leaves into the
    // motion target, skinning the current + previous pose so each pixel gets its
    // true screen motion. Depth-tested (LEQUAL, no write) against the complete
    // scene depth. Caller owns render begin/end + the negative-height viewport
    // (VK::MotionVec::ExecuteDynamic). prevVP = previous frame's view-proj.
    // curVP/prevVP are UNJITTERED (jitter-free MV); jitterNdcX/Y (D3D-NDC, 0 when DLSS
    // off) is re-applied to gl_Position so depth still bit-matches the jittered forward geometry.
    // minRange > 0 (r_mv_prepass): draw ONLY the skeletons FARTHER than that, because
    // the prepass already wrote the near ones' motion in the raster that laid their
    // depth. The prepass keeps NPCs within Skinned_PrepassRange() as AO occluders and
    // no farther, so the two halves have to complement each other exactly — a far NPC
    // is not in the prepass depth at all, and without this pass its pixels would carry
    // the motion of whatever the reconstruction found behind it.
    void Skinned_RenderMotion(VkCommandBuffer cmd, const Fmatrix& curVP, const Fmatrix& prevVP,
                              float jitterNdcX, float jitterNdcY, float minRange = 0.0f);

    // The camera-distance cut the depth prepass applies to NPCs (AO is short-range).
    float Skinned_PrepassRange();

    // First-person HUD weapon MV overlay: same as above for the HUD list, using the
    // HUD-FOV projection (cur/prev) at the near-depth viewport [0,0.02] the forward
    // HUD pass used. Fixes the viewmodel ghosting under DLSS/TAA. Caller owns begin/end.
    void Skinned_RenderMotionHud(VkCommandBuffer cmd, const VkExtent2D& ext,
                                 const Fmatrix& curHudVP, const Fmatrix& prevHudVP,
                                 float jitterNdcX, float jitterNdcY);

    // Glass refraction, kinematics half (r_glass_refr): re-draw this frame's glass
    // leaves into the heat-haze distortion pass (called by Pass_Particles phase 3).
    void Skinned_RenderGlassDistort(VkCommandBuffer cmd, const Fmatrix& viewProj, float strength);

    // Late glass, kinematics half: the world glass leaves (models\window -- panes, the
    // invisible bloodsucker) Pass_Skinned held back, drawn by Pass_WorldGlass after the
    // opaque world + sky + water. Caller owns the overlay scope.
    bool Skinned_HasLateGlass();
    void Skinned_RenderGlassLate(VkCommandBuffer cmd, const Fmatrix& viewProj);

    // --- VSM skinned casters (vk_vsm consumes these to render NPC shadows into the
    // virtual shadow atlas). One entry per visible world skinned leaf this frame.
    struct VsmSkinnedCaster {
        Fvector      sphere_P;      // world-space bounding sphere (skeleton bound)
        float        sphere_R;
        u32          index_count, ib_first;
        s32          first_vertex;
        VkBuffer     vb, ib;
        VkIndexType  iType;
        u32          stride;        // vertex stride (36/40/44) → pipeline key + vertex input
        u32          base_bone, bone_count, skin_mode;
    };
    // Collect this frame's world skinned leaves as VSM casters (ensures bones are
    // uploaded first). HUD never casts. Append-only into `out` (not cleared here).
    void Skinned_CollectCasters(xr_vector<VsmSkinnedCaster>& out);

    // The shared world-space bone SSBO set (set 0, binding 0) + its layout — vk_vsm's
    // skinned-page pipeline binds the SAME set so baseBone indexes the same matrices.
    VkDescriptorSet       Skinned_GetBoneSet();
    VkDescriptorSetLayout Skinned_GetBoneSetLayout();

    // Build the vertHW skinned vertex input for `stride` (6 attrs) — vk_vsm reuses it
    // for the skinned-page pipeline so the layout matches the mesh buffers exactly.
    void Skinned_BuildVertexInput(u32 stride, VkVertexInputBindingDescription& binding,
                                  VkVertexInputAttributeDescription attrs[6]);
}
