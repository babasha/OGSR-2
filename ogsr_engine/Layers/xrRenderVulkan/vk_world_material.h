// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - World-pass material cache.
//
// Phase 5: maps a diffuse-texture name to a descriptor set the world pass
// can bind. Deliberately small (one combined image sampler per material) and
// independent of the parked CMaterial monolith infrastructure — that one
// pulls deferred-renderer state we don't want yet. When deferred lands this
// cache merges into it.

#pragma once
#include "vk_core.h"

namespace VK {

class CVulkanTexture;

struct WorldMaterial
{
    // Base diffuse (set 0, binding 0).
    CVulkanTexture* tex      = nullptr;
    VkImageView     view     = VK_NULL_HANDLE;
    VkSampler       sampler  = VK_NULL_HANDLE;

    // R4-style detail texture (set 0, binding 1). Always non-null after
    // GetOrCreate — materials with no .thm-declared detail point at the
    // shared 1×1 grey fallback. detailScale = 0 disables UV scaling, the
    // grey fallback's 0.5 sample makes `2 * base * 0.5 = base` a no-op.
    VkImageView     view_detail  = VK_NULL_HANDLE;
    float           detailScale  = 0.0f;

    // Lightmap (set 0, binding 2). Bound only on lmap-style materials
    // (tcOffset==24 with a 3rd texture slot named "lmap*" in the level
    // shader). Other materials get a 1×1 white fallback so the descriptor
    // is always valid; the vert-lit fragment shader simply doesn't read it.
    VkImageView     view_lmap    = VK_NULL_HANDLE;

    // Heightmap tessellation (R4 TESS_HM): set 0, binding 3 = `<bump>#.dds`
    // whose ALPHA is the displacement height, sampled by the tessellation
    // evaluation shader. `tessellated` is the opt-in: true only when the
    // diffuse .thm declares a bump association AND the # texture loads AND
    // the material is plain opaque (no aref/wmark — those must match the
    // flat depth prepass exactly). Others bind a 1×1 zero-alpha fallback.
    bool            tessellated  = false;
    // Binding-3 view, kept so a streaming refresh can rewrite a FRESH set with
    // all five bindings (the old set stays untouched for in-flight frames).
    VkImageView     view_bump    = VK_NULL_HANDLE;

    // Set 0, binding 4 = `<bump>.dds`: the material's tangent NORMAL (R4 packing:
    // n = tex.wzy*2-1) and its GLOSS in .x. Unlike the `#` height above this is NOT
    // gated on tessellation eligibility — a normal map changes shading, never
    // coverage, so alpha-tested and decal materials take it too. `hasBumpN` is
    // false when the 1×1 flat-normal/zero-gloss fallback is bound, which lets the
    // shader variant compile the whole branch out.
    VkImageView     view_bumpn   = VK_NULL_HANDLE;
    bool            hasBumpN     = false;
    // The texture BEHIND view_bumpn. Needed because `<bump>` maps are mip-streamed
    // (TexStreamClass::Bump): when the streamer swaps a bump's residency it hands the
    // rebind callback a CVulkanTexture*, and without this back-pointer the callback
    // could only match base diffuse — leaving every bump descriptor dangling at the
    // first promotion. Bumps are SHARED across materials (cached by name), so one
    // swap legitimately rewrites many sets.
    CVulkanTexture* tex_bumpn    = nullptr;

    VkDescriptorSet set      = VK_NULL_HANDLE;
    float           alphaRef = -1.0f;
    shared_str      name;          // diffuse texture name (for classification, e.g. tree trunk vs crown)

    // GPU-feedback slot of the base diffuse (TextureStreamer::GetFeedbackSlot).
    // Pushed to the world FS at offset 116; 0xFFFFFFFF = no feedback (shader skips).
    u32             streamID = 0xFFFFFFFFu;

    // --- Terrain splatting (R4 CBlender_BmmD) ---
    // When the diffuse name starts with "terrain\", this material also gets an
    // 11-binding terrain set {base, mask, dt_r..dt_a, lmap, dn_r..dn_a} and is
    // rendered with the terrain pipeline. dn_* are the <detail>_bump tangent
    // normal maps (R4 CBlender_BmmD). `terrainSet` is VK_NULL_HANDLE for
    // non-terrain materials, which keep the plain 3-binding `set` above.
    bool            isTerrain  = false;
    VkDescriptorSet terrainSet = VK_NULL_HANDLE;
    // Mask-less terrain only: the one-hot splat channel synthesized from the
    // level shader name (0=R grass, 1=G asphalt, 2=B earth, 3=A yantar).
    // 255 = the material has a REAL `_mask` (never touched by the mask bake).
    u8              terrainChannel = 255;

    // Baked level decal (newspapers / dirt overlays — level shader effects\
    // wallmark*): geometry coplanar with the surface beneath. Rendered last,
    // alpha-blended, no depth write, negative depth bias (else z-fight).
    bool            isWmark    = false;

    // Translucent glass pane (glas\ textures / "glass" shaders; marked by
    // alphaRef == -2 at GetOrCreate). Uses the wmark blend pipeline BUT draws
    // in the LATE glass flush (Pass_WorldGlass) — after the whole opaque world,
    // else later-drawn geometry behind the pane overwrites the blended pixels.
    bool            isGlass    = false;

    // Emissive-additive (level `effects\glow` halos, `selflight` model parts;
    // marked by alphaRef == -3): additive blend, unlit FS output, drawn in the
    // late flush like glass. The lamp/projector "shining" look in R4.
    bool            isEmisAdd  = false;

    // Lit-blend (lightplanes beams; alphaRef == -4): R4 model_def_lq — LIT
    // colour, plain srcalpha blend (the wmark pipeline), alpha = tex.a·fog².
    bool            isLitBlend = false;

    // WATER body (level shader `effects\water*`; alphaRef == -5). Not drawn by
    // the world pipelines at all: Pass_Water owns it (own pipeline, own shader,
    // premultiplied blend over the finished opaque scene). Shares the wmark
    // marker so it stays out of the depth prepass and the shadow-caster bins —
    // a water plane must not occlude its own bottom, nor cast a shadow onto it.
    bool            isWater    = false;
};

namespace WorldMaterialCache {

bool Init();
void Destroy();

// Call on level_Load with a per-level tag (the resolved $level$ path). Lightmap
// names (lmap#01…) REPEAT across levels while the .dds behind them differ, and
// this cache deliberately survives level changes (persistent visuals hold raw
// WorldMaterial* — see the level-transition memory) — so lightmap textures and
// lmap-bearing material keys are namespaced by this tag; without it a level
// change binds the PREVIOUS level's lightmaps ("baked" light/dark patches that
// ignore the sun). Materials WITHOUT a lightmap (weapons/NPC/props) stay
// globally keyed and shared across levels.
void SetLevelTag(const char* tag);

// Lazy: loads `<diffuse>.dds` (and `<lmap_name>.dds` if non-null) from
// $game_textures$/$level$ on first call. Returns the cache's default
// (1×1 white) on failure or empty diffuse. `lmap_name` may be null/empty
// for non-lightmapped materials → binding 2 falls back to 1×1 white.
// Cache keys both names together so the same diffuse can pair with
// different lmaps.
// `shader_name` (the LEVEL shader, e.g. "levels\pripyat_asfalt") matters only
// for terrain diffuses with NO `_mask` texture: the vanilla-SoC convention
// regionalizes terrain by SHADER, so the material synthesizes a one-hot splat
// mask from the shader name (asfalt→asphalt channel, grass→grass, ...) and is
// keyed per shader — without it every region blends all four details evenly
// ("каша" ground on mask-less maps like pripyat_full).
WorldMaterial* GetOrCreate(const char* diffuse_name, const char* lmap_name, float alphaRef, bool wmark = false,
                           const char* shader_name = nullptr);


// Kick off the parallel texture prefetch for this level (VK::TexPrefetch). Pass
// the level shader table's base + lightmap names; this cache owns the rules that
// turn a base name into the actual file set (.thm detail, bump normal + height,
// terrain mask), so the expansion lives here rather than in the loader.
// Purely an I/O head start: GetOrCreate below is untouched and still decides what
// is really loaded, so prefetching a file nothing asks for wastes a read and
// nothing else. Pair with VK::TexPrefetch::Stop() after the visual walk.
void StartTexturePrefetch(xr_vector<shared_str>&& diffuseNames, xr_vector<shared_str>&& lmapNames,
                          const char* leadFile);

VkDescriptorSetLayout GetSetLayout();
VkDescriptorSetLayout GetTerrainSetLayout();   // 7-binding terrain splat set
VkSampler             GetSampler();
WorldMaterial*        GetDefault();
void                  SetViews(const WorldMaterial* m, VkImageView out[5]);   // r_world_bindless: WriteSet's five views

// Per-frame tick (called from CRender::Begin, after the frame fence, before any
// recording): recycles descriptor sets retired by streaming refreshes once no
// in-flight frame can still reference them.
void FrameTick();

// Rewrite binding 1 (splat mask) of every MASK-LESS terrain material's set to
// `view` (the TerrainMask bake). Call only while the terrain sets are not
// referenced by in-flight work (level load end).
void RebindTerrainMasks(VkImageView view);

}  // namespace WorldMaterialCache
}  // namespace VK
