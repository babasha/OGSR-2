// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_world_material.h"
#include "vk_texture.h"
#include "vk_texture_stream.h"   // TextureStreamer rebind hook (dynamic streaming)
#include "vk_descriptors.h"      // VK::DescriptorWriter
#include "vk_clk.h"              // rdtsc counters - 388k calls per load, CTimer truncates
#include "HW_Vulkan.h"

#include "../xrRender/ETextureParams.h"   // STextureParams::Load + flDiffuseDetail flag

// r_bump — material normal/gloss strength. 0 also SKIPS loading `<bump>.dds` here, so
// the cvar gates VRAM cost and not just shading (see the load site below).
extern float ps_r_bump;

// The "<bump>#" height texture (alpha = height) feeds only tessellation and POM. With all three off (the Switch preset) nothing
// samples it, yet every material load still read it and repacked BC3->BC4 on the CPU: ~40 ms per texture on the A57, which
// made a weapon's model_Create 800-1600 ms (Switch v78 log) -> 200-500 ms frame_move stalls.
// Read at material-load time: flipping r_tess/r_pom live needs a level reload to bring the heights back.
extern float ps_r_tess;
extern int   ps_r_pom, ps_r_pom_terrain;
static inline bool HeightTexNeeded() { return ps_r_tess > 0.0f || ps_r_pom != 0 || ps_r_pom_terrain != 0; }

// r_thm_cache -- see vk_console_min.cpp. Global scope: a namespace-scope extern
// mangles differently and silently fails to bind.
extern int ps_r_thm_cache;

#include <unordered_map>
#include <string>
#include <array>
#include <set>
#include <unordered_set>   // prefetch base/lmap dedup

#include <vector>
#include <mutex>
#include <condition_variable>
#include <atomic>

// An EXTRA texture search root, set by an editor host. The editor's content lives in ITS
// own gamedata, not the engine's: a level authored against other assets (a ported map, a
// mod's source tree) resolves its geometry through Ed_AddModelFile but would still miss
// every texture, and a missing base map silently becomes the 1x1 white default — an
// all-white terrain. Declared at file scope (inside `namespace VK` it would bind to a
// non-existent VK::VKEditor).
namespace VKEditor
{
static string_path s_texRoot = {0};

// Anything cached during Init that depends on the host's asset root has to be
// re-attempted once that root finally arrives — see the note in SetTextureRoot.
void RetryTerrainBlenderMap();

void SetTextureRoot(const char* dir)
{
    if (dir && dir[0])
        strcpy_s(s_texRoot, sizeof(s_texRoot), dir);
    else
        s_texRoot[0] = 0;

    // ORDERING: the host hands us this root AFTER the engine has booted, and
    // WorldMaterial::Init already ran by then — it loads the shaders.xr blender map
    // once and latches, roughly half a second too early to see the root. Measured on
    // a real run: blender map at 19.343, root at 19.893. Model materials are built
    // later still (25.1+), so THEY see it fine; only the Init-time load loses the
    // race. Re-attempt it here rather than leaving terrain on global detail defaults.
    RetryTerrainBlenderMap();
}

const char* TextureRoot() { return s_texRoot[0] ? s_texRoot : nullptr; }
} // namespace VKEditor

namespace VK { namespace WorldMaterialCache {

namespace {
    VkDescriptorSetLayout                              s_SetLayout    = VK_NULL_HANDLE;
    VkDescriptorPool                                   s_Pool         = VK_NULL_HANDLE;
    VkSampler                                          s_Sampler      = VK_NULL_HANDLE;
    std::unordered_map<std::string, WorldMaterial*>    s_Cache;

    // Per-level namespace for lightmap-related cache keys (see SetLevelTag in
    // the header): lmap names repeat across levels but resolve to DIFFERENT
    // $level$ .dds files, and this cache outlives level changes by design.
    std::string s_LevelTag;
    WorldMaterial*                                     s_Default      = nullptr;

    // --- Streaming-refresh set recycling ------------------------------------
    // A texture-streaming swap never touches a live descriptor set (in-flight
    // frames still sample through it). Instead the material gets a FRESH set and
    // the old one retires here; FrameTick returns it to the free list once the
    // fence horizon passes, and AllocateSet reuses free-list sets before dipping
    // into the pool — so streaming churn doesn't leak pool capacity.
    struct RetiredSet { u32 frame; VkDescriptorSet set; };
    std::vector<RetiredSet>      s_RetiredSets;
    std::vector<VkDescriptorSet> s_FreeSets;
    std::mutex                   s_StreamMutex;   // guards the 3 containers above

    // Base-diffuse cache, keyed by RESOLVED PATH and shared by reference — the
    // sibling every other texture role already had. A material is identified by
    // (diffuse|lmap|shader), and one diffuse legitimately pairs with many lmaps, so
    // creating the image inside GetOrCreate meant one .dds became as many GPU images
    // as it had material keys. ⚠ MEASURED on Pripyat before this cache existed:
    // 617 files resident 2+ times, 809 redundant copies, 780 MB — a QUARTER of the
    // level's 3136 MB texture footprint, with `build_details.dds` alone held 29
    // times (298 MB). Invisible in every other metric: the class totals count the
    // copies as legitimate residency.
    // Sharing is safe because the swap path was already written for it — the
    // streamer's rebind callback walks every material and rewrites each set whose
    // base or bumpn matches the swapped texture (that is how `<bump>` maps, shared
    // by name since day one, have always worked).
    std::unordered_map<std::string, CVulkanTexture*>   s_DiffuseTexCache;

    // Detail-texture cache (shared by reference across materials).
    std::unordered_map<std::string, CVulkanTexture*>   s_DetailTexCache;
    CVulkanTexture*                                    s_GreyDetail   = nullptr;  // 1×1 0.5-grey fallback

    // Lightmap cache and 1×1 white fallback (vert-lit / non-lightmapped
    // materials still need a valid binding; white preserves albedo).
    std::unordered_map<std::string, CVulkanTexture*>   s_LmapTexCache;
    CVulkanTexture*                                    s_WhiteLmap    = nullptr;

    // Bump-error textures (`<bump>#.dds`, alpha = height) for tessellation
    // displacement, shared by reference. Fallback is 1×1 with ZERO alpha —
    // a TES sampling it produces zero displacement.
    std::unordered_map<std::string, CVulkanTexture*>   s_BumpTexCache;
    CVulkanTexture*                                    s_FlatBump     = nullptr;

    // Material NORMAL+GLOSS (`<bump>.dds`, R4 packing: normal = tex.wzy*2-1,
    // gloss = tex.x), binding 4. Fallback is 1x1 (13,255,128,128) = a flat tangent normal
    // with the MEASURED median gloss of the installed content, so a material without a
    // bump shades like its neighbours that have one (see the note at its creation).
    std::unordered_map<std::string, CVulkanTexture*>   s_BumpNTexCache;
    CVulkanTexture*                                    s_FlatBumpN    = nullptr;

    // --- Terrain splatting resources (R4 CBlender_BmmD) ---
    // Separate 7-binding set {base, mask, dt_r, dt_g, dt_b, dt_a, lmap} +
    // its own pool, plus a shared white 1×1 mask fallback (normalized → even
    // blend) and the 4 default channel-detail textures (grass/asphalt/earth/
    // gravel). Detail/mask textures are shared by reference across materials.
    VkDescriptorSetLayout                              s_TerrainSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool                                   s_TerrainPool      = VK_NULL_HANDLE;
    CVulkanTexture*                                    s_WhiteMask        = nullptr;  // 1×1 white
    // One-hot 1×1 splat masks (R=grass, G=asphalt, B=earth, A=yantar — the kDet
    // channel order). Used for mask-LESS terrain regionalized by SHADER name
    // (vanilla SoC): pripyat_asfalt gets the pure-asphalt mask instead of the
    // white even-blend that turns the whole ground into detail mush.
    CVulkanTexture*                                    s_ChannelMask[4]   = {};
    CVulkanTexture*                                    s_TerrainDetail[4] = {};       // R/G/B/A diffuse details
    CVulkanTexture*                                    s_TerrainNormal[4] = {};       // R/G/B/A <detail>_bump normal maps
    CVulkanTexture*                                    s_FlatNormal       = nullptr;  // 1×1 (0,0,1) tangent normal fallback
    CVulkanTexture*                                    s_TerrainHeight[4] = {};       // R/G/B/A <detail>_height maps (SSFX terrain POM)
    CVulkanTexture*                                    s_FlatHeight       = nullptr;  // 1×1 mid-grey height fallback (uniform -> no relief)
    std::unordered_map<std::string, CVulkanTexture*>   s_TerrainDetCache;             // by name (diffuse + normal + height + mask)

    // Resolved file path: $game_textures$\\<name>.dds
    bool ResolveTexturePath(const char* name, string_path& out)
    {
        if (!name || !name[0]) return false;
        string_path leaf;
        xr_sprintf(leaf, "%s.dds", name);
        // World diffuse textures are usually global ($game_textures$), but some
        // levels pack their terrain base into the level's own archive alongside
        // the lightmaps (e.g. Garbage: shader wants terrain\terrain_garbage +
        // terrain\terrain_garbage_lm, and only the _lm lived in $level$ — the
        // base was missed here and the ground rendered as the white default).
        // Mirror the lmap / detail / LOD-atlas loaders: try the global store
        // first, then fall back to $level$.
        FS.update_path(out, "$game_textures$", leaf);
        if (FS.exist(out)) return true;
        FS.update_path(out, "$level$", leaf);
        if (FS.exist(out)) return true;

        // Last resort: an editor host's own texture store. Tested on the FILESYSTEM, not
        // through FS.exist — that root is outside our gamedata so it was never scanned
        // into the VFS registry. FS.r_open still opens an absolute path that exists.
        if (const char* root = VKEditor::TextureRoot())
        {
            strconcat(sizeof(out), out, root, "\\", leaf);
            if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES)
                return true;
        }
        return false;
    }

    VkDescriptorSet AllocateSet()
    {
        {   // Recycled set from a past streaming refresh? Fully rewritten by the
            // caller, so reuse is safe once it cleared the fence horizon.
            std::lock_guard<std::mutex> lk(s_StreamMutex);
            if (!s_FreeSets.empty()) {
                VkDescriptorSet s = s_FreeSets.back();
                s_FreeSets.pop_back();
                return s;
            }
        }
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = s_Pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &s_SetLayout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult r = vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, &set);
        if (r != VK_SUCCESS) {
            Msg("![VK WorldMaterial] vkAllocateDescriptorSets failed (%d)", r);
            return VK_NULL_HANDLE;
        }
        return set;
    }

    void WriteSet(VkDescriptorSet set, VkImageView baseView,
                  VkImageView detailView, VkImageView lmapView, VkImageView bumpxView,
                  VkImageView bumpnView)
    {
        const VkImageView views[5] = { baseView, detailView, lmapView, bumpxView, bumpnView };
        VK::DescriptorWriter w(set);
        for (u32 i = 0; i < 5; ++i) w.ImageSampler(i, views[i], s_Sampler);
        w.Flush();
    }

    // ---- One load protocol for every texture cache ---------------------------
    // The five loaders below used to be five copies of "look in my map, else build
    // it", each on the loading thread and each unsynchronised. Both halves stopped
    // being true: prefetch workers now build textures ahead of the walk
    // (r_tex_materialize), so the maps are shared, and a file already being built
    // elsewhere must be WAITED for instead of built twice. The wait is the point --
    // it is what lets a worker do the walk's work before the walk gets there.
    //
    // The lock closes an older hole too: spawns create visuals (and so materials)
    // from the secondary thread, and these maps never had one.
    std::mutex                                    s_texMx;
    std::condition_variable                       s_texCv;
    std::set<std::pair<const void*, std::string>> s_texInflight;   // (cache, key) under construction
    std::atomic<u32>                              s_texWaits{0};   // callers that waited on another thread
    std::atomic<u64>                              s_texWaitClk{0};
    std::atomic<u32>                              s_texBuilt{0};   // textures actually built through here

    // GetOrCreate's miss path reported one number (`create`), and it is not one
    // thing: five texture loads, a .thm lookup, a terrain residency heal that can
    // REBUILD an image, two per-material log lines, and the descriptor set. 2028
    // misses a load, and which of those owns the milliseconds decides whether
    // moving texture building off this thread is worth anything at all.
    std::atomic<u64> s_mkDiffuseClk{0}, s_mkTerrainClk{0}, s_mkThmClk{0}, s_mkBumpxClk{0},
                     s_mkBumpnClk{0}, s_mkLmapClk{0}, s_mkDiagClk{0}, s_mkSetClk{0};

    CVulkanTexture* FindInCache(std::unordered_map<std::string, CVulkanTexture*>& cache,
                                const std::string& key)
    {
        std::lock_guard<std::mutex> lk(s_texMx);
        auto it = cache.find(key);
        return it != cache.end() ? it->second : nullptr;
    }

    // `full == nullptr` means "the file does not exist" -- the fallback is cached
    // under the key exactly as the old inline miss-path did, so nothing re-stats it.
    CVulkanTexture* LoadIntoCache(std::unordered_map<std::string, CVulkanTexture*>& cache,
                                  std::string key, const char* full, CVulkanTexture* fallback,
                                  TexStreamClass klass, TexColorSpace colorSpace, bool alphaOnly)
    {
        const auto id = std::make_pair((const void*)&cache, key);
        {
            std::unique_lock<std::mutex> lk(s_texMx);
            for (;;) {
                auto it = cache.find(key);
                if (it != cache.end()) return it->second;
                if (s_texInflight.insert(id).second) break;   // claimed: ours to build
                const u64 _w0 = CPU::GetCLK();
                s_texCv.wait(lk);
                s_texWaitClk += CPU::GetCLK() - _w0;
                s_texWaits.fetch_add(1, std::memory_order_relaxed);
            }
        }

        CVulkanTexture* out = fallback;
        if (full && full[0]) {
            auto* tex = xr_new<CVulkanTexture>();
            tex->SetAlphaOnly(alphaOnly);
            if (tex->LoadDDS(full, /*applyBCSwizzle*/ false, klass, colorSpace)) {
                out = tex;
                s_texBuilt.fetch_add(1, std::memory_order_relaxed);
            }
            else
                xr_delete(tex);
        }

        {
            std::lock_guard<std::mutex> lk(s_texMx);
            cache.emplace(std::move(key), out);
            s_texInflight.erase(id);
        }
        s_texCv.notify_all();
        return out;
    }

    CVulkanTexture* GetOrLoadLmapTex(const char* lmap_name)
    {
        if (!lmap_name || !lmap_name[0]) return s_WhiteLmap;

        // Level-tagged key: the same "lmap#01" on another level is a DIFFERENT
        // texture (loaded from that level's $level$ dir below).
        std::string key(s_LevelTag);
        key.push_back('|');
        key.append(lmap_name);
        if (CVulkanTexture* hit = FindInCache(s_LmapTexCache, key)) return hit;

        // Lightmaps live alongside the level data ($level$/<lmap>.dds).
        string_path leaf;
        xr_sprintf(leaf, "%s.dds", lmap_name);
        string_path full;
        FS.update_path(full, "$level$", leaf);
        if (!FS.exist(full)) {
            FS.update_path(full, "$game_textures$", leaf);
            if (!FS.exist(full))
                return LoadIntoCache(s_LmapTexCache, std::move(key), nullptr, s_WhiteLmap,
                                     TexStreamClass::Lmap, TexColorSpace::Data, false);
        }
        return LoadIntoCache(s_LmapTexCache, std::move(key), full, s_WhiteLmap,
                             TexStreamClass::Lmap, TexColorSpace::Data, false);
    }

    // Everything we pull out of a diffuse's `<base>.thm`: the R4 detail
    // descriptor (texture + UV scale) and the bump association (tessellation
    // height comes from `<bump>#.dds`).
    struct THMInfo
    {
        std::string detail_name;
        float       detail_scale = 0.0f;
        bool        has_detail   = false;
        std::string bump_name;
        bool        has_bump     = false;
    };

    // Looks up `<base>.thm` in $game_textures$ (then $level$, then an editor host's
    // own texture store) and fills `out`. Returns false when the .thm is
    // missing/unreadable — caller falls back to the grey detail / flat bump samplers.
    // Counter + cache, see r_thm_cache. Both call sites (the prefetch expansion on
    // 16 workers and the walk's material creation on the loading thread) ask for the
    // SAME base names, so this is read twice per material by construction.
    std::mutex                                  s_thmMx;
    std::unordered_map<std::string, std::pair<THMInfo, bool>> s_thmCache;
    std::atomic<u64> s_thmClk{0};
    std::atomic<u32> s_thmCalls{0}, s_thmHits{0};

    bool LookupTHM_Read(const char* base_name, THMInfo& out)
    {
        if (!base_name || !base_name[0]) return false;

        string_path file_nm;
        xr_sprintf(file_nm, "%s.thm", base_name);

        string_path full;
        FS.update_path(full, "$game_textures$", file_nm);
        IReader* F = FS.r_open(full);
        if (!F) {
            FS.update_path(full, "$level$", file_nm);
            F = FS.r_open(full);
        }
        if (!F) {
            // SAME fallback ResolveTexturePath already had for the .dds — and its
            // absence here was a real defect, not a missing nicety. In a host-driven
            // editor the diffuse resolved through the editor root while its .thm did
            // NOT, so EVERY material silently lost its detail texture, its detail
            // scale and its bump/height: surfaces rendered as a stretched base layer
            // with no close-range detail. Tested on the FILESYSTEM, not FS.exist —
            // that root sits outside our gamedata and was never scanned into the VFS.
            if (const char* root = VKEditor::TextureRoot()) {
                strconcat(sizeof(full), full, root, "\\", file_nm);
                if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES)
                    F = FS.r_open(full);
            }
        }
        if (!F) return false;

        // .thm structure: outer chunk THM_CHUNK_TYPE (0x0813) carries the
        // texture-class id (Image / Terrain / NormalMap). STextureParams::Load
        // walks the TEXTUREPARAM / DETAIL_EXT / MATERIAL / BUMP / etc. chunks.
        if (!F->find_chunk_thm(THM_CHUNK_TYPE, base_name)) {
            FS.r_close(F);
            return false;
        }
        F->r_u32();  // ETType — read for parity, value not needed

        STextureParams tp{};
        tp.Load(*F, base_name);
        FS.r_close(F);

        if (tp.detail_name.size() > 0 &&
            tp.flags.is_any(STextureParams::flDiffuseDetail | STextureParams::flBumpDetail)) {
            out.detail_name  = tp.detail_name.c_str();
            out.detail_scale = tp.detail_scale;
            out.has_detail   = true;
        }

        if (tp.bump_name.size() > 0 &&
            (tp.bump_mode == STextureParams::tbmUse || tp.bump_mode == STextureParams::tbmUseParallax)) {
            out.bump_name = tp.bump_name.c_str();
            out.has_bump  = true;
        }

        return true;
    }

    bool LookupTHM(const char* base_name, THMInfo& out)
    {
        if (!base_name || !base_name[0]) return false;
        const u64 _c0 = CPU::GetCLK();
        s_thmCalls.fetch_add(1, std::memory_order_relaxed);

        if (!ps_r_thm_cache) {
            const bool ok = LookupTHM_Read(base_name, out);
            s_thmClk.fetch_add(CPU::GetCLK() - _c0, std::memory_order_relaxed);
            return ok;
        }

        std::string key(base_name);
        for (char& c : key) c = (char)tolower((unsigned char)c);
        key.push_back('|');
        key.append(s_LevelTag);   // a .thm can resolve out of $level$

        {
            std::lock_guard<std::mutex> lk(s_thmMx);
            auto it = s_thmCache.find(key);
            if (it != s_thmCache.end()) {
                s_thmHits.fetch_add(1, std::memory_order_relaxed);
                out = it->second.first;
                s_thmClk.fetch_add(CPU::GetCLK() - _c0, std::memory_order_relaxed);
                return it->second.second;
            }
        }

        // Read outside the lock: two workers racing on the same base do the work
        // twice and agree on the answer, which is cheaper than serialising 2400 reads.
        THMInfo parsed{};
        const bool ok = LookupTHM_Read(base_name, parsed);
        {
            std::lock_guard<std::mutex> lk(s_thmMx);
            s_thmCache.emplace(std::move(key), std::make_pair(parsed, ok));
        }
        out = parsed;
        s_thmClk.fetch_add(CPU::GetCLK() - _c0, std::memory_order_relaxed);
        return ok;
    }

    CVulkanTexture* GetOrLoadDetailTex(const std::string& detail_name)
    {
        if (detail_name.empty()) return s_GreyDetail;

        if (CVulkanTexture* hit = FindInCache(s_DetailTexCache, detail_name)) return hit;

        // Detail textures live next to base diffuses under $game_textures$.
        string_path leaf;
        xr_sprintf(leaf, "%s.dds", detail_name.c_str());
        string_path full;
        FS.update_path(full, "$game_textures$", leaf);
        // A miss is cached under the key as well, so nothing keeps stat'ing the FS.
        return LoadIntoCache(s_DetailTexCache, detail_name, FS.exist(full) ? full : nullptr,
                             s_GreyDetail, TexStreamClass::Detail, TexColorSpace::Data, false);
    }

    // Generic cached loader for a named .dds in $game_textures$ (then $level$).
    // Returns `fallback` (never null) on miss so descriptors stay valid.
    // `colorSpace` is explicit at every call: this ONE cache serves four different roles
    // (terrain colour detail, its _bump normals, its _height maps and the splat _mask),
    // which is exactly why TexStreamClass could not be reused to answer colour-vs-data —
    // all four share TexStreamClass::Terrain. Keys stay distinct via the name suffixes.
    CVulkanTexture* GetOrLoadGameTex(std::unordered_map<std::string, CVulkanTexture*>& cache,
                                     const char* name, CVulkanTexture* fallback,
                                     TexColorSpace colorSpace = TexColorSpace::Data,
                                     TexStreamClass klass = TexStreamClass::Terrain,
                                     bool alphaOnly = false)
    {
        if (!name || !name[0]) return fallback;
        std::string key(name);
        if (CVulkanTexture* hit = FindInCache(cache, key)) return hit;

        string_path leaf, full;
        xr_sprintf(leaf, "%s.dds", name);
        FS.update_path(full, "$game_textures$", leaf);
        if (!FS.exist(full)) {
            FS.update_path(full, "$level$", leaf);
            if (!FS.exist(full))
                return LoadIntoCache(cache, std::move(key), nullptr, fallback, klass, colorSpace, alphaOnly);
        }
        // Shared loader for terrain detail/normal/height + mask + bump maps. The CLASS
        // decides residency policy: Terrain = tracked, budget-fit only, never streamed
        // (a handful of tiled textures); Bump = mip-streamed like base diffuse, because
        // there is one per MATERIAL and a big level has thousands (Pripyat: 1591).
        // alphaOnly = "we sample .a and nothing else" → the loader may halve it to BC4.
        return LoadIntoCache(cache, std::move(key), full, fallback, klass, colorSpace, alphaOnly);
    }

    // Local mirror of CBlender_DESC's on-disk layout (the real header drags in the
    // R4-only Blender_Recorder, which won't compile here). pack(4) + field order
    // must match xrRender\blenders\Blender.h exactly so sizeof == the serialized
    // record (u64 + 128 + 32 + 4 + 2 -> 176 bytes under pack(4)).
#pragma pack(push, 4)
    struct BmmdDescRaw { CLASS_ID CLS; char cName[128]; char cComputer[32]; u32 cTime; u16 version; };
#pragma pack(pop)
    static const CLASS_ID kCLS_BmmD = MK_CLSID('B', 'm', 'm', 'D', 'o', 'l', 'd', ' ');

    // ----- Per-shader terrain detail sets from shaders.xr (CBlender_BmmD) -------
    // Each terrain shader (e.g. "levels\l01_escape_grass") is a B_BmmD blender that
    // names its own 4 channel-detail textures (oR/oG/oB/oA). The VK renderer never
    // parsed shaders.xr, so it hardcoded one global set. This reads the blender DB
    // (chunk 2) and maps blender-name(lower) -> {R,G,B,A}. Binary layout verified
    // against ResourceManager_Loader.cpp + IBlenderXr::Load + Blender_BmmD::Load.
    std::unordered_map<std::string, std::array<std::string, 4>> s_BlenderDet;
    bool s_BlenderDetLoaded = false;

    void LoadTerrainBlenderMap()
    {
        if (s_BlenderDetLoaded) return;
        s_BlenderDetLoaded = true;

        // shaders.xr sits at the gamedata root inside gamedata.db_base_configs.
        // Try the canonical alias forms (open directly — more robust than exist()).
        IReader* F = FS.r_open("$game_data$", "shaders.xr");
        if (!F) F = FS.r_open("$fs_root$", "gamedata\\shaders.xr");
        if (!F) F = FS.r_open("$game_config$", "..\\shaders.xr");
        if (!F) {
            // Editor host: its gamedata is the SDK's, and shaders.xr sits at that
            // root — one level above the texture store it hands us. A game install
            // may not carry a loose copy at all (Gunslinger does not), so without
            // this the editor fell back to global detail defaults for terrain.
            if (const char* root = VKEditor::TextureRoot()) {
                string_path p;
                strconcat(sizeof(p), p, root, "\\..\\shaders.xr");
                if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES)
                    F = FS.r_open(p);
            }
        }
        if (!F) {
            Msg("[VK Terrain] shaders.xr not found -> per-shader detail sets OFF (global defaults)");
            return;
        }
        Msg("[VK Terrain] shaders.xr loaded -> per-shader detail sets ON");

        // Compressed-library guard (mirror ResourceManager_Loader): read 8-byte id.
        char id8[8];
        F->r(id8, 8);
        if (0 == strncmp(id8, "shENGINE", 8)) {
            Msg("![VK Terrain] shaders.xr compressed (shENGINE) -> using global defaults");
            FS.r_close(F);
            return;
        }

        // Property stream helpers: each prop = u32 type + stringZ name + sizeof(data).
        auto skip_marker = [](IReader& r) { r.r_u32(); r.skip_stringZ(); };
        auto skip_prop   = [](IReader& r, u32 n) { r.r_u32(); r.skip_stringZ(); r.advance(n); };
        auto read_str64  = [](IReader& r, string64& out) { r.r_u32(); r.skip_stringZ(); r.r(out, sizeof(string64)); };

        int total = 0, bmmd = 0;
        if (IReader* fs = F->open_chunk(2)) {
            IReader* chunk; int cid = 0;
            while ((chunk = fs->open_chunk(cid)) != nullptr) {
                ++total;
                BmmdDescRaw desc;
                chunk->r(&desc, sizeof(desc));
                if (desc.CLS == kCLS_BmmD && desc.version >= 3) {
                    string64 R{}, G{}, B{}, A{};
                    skip_marker(*chunk);                    // "General"
                    skip_prop(*chunk, 12u);                 // oPriority  (xrP_INTEGER = 3*int)
                    skip_prop(*chunk, 4u);                  // oStrictSorting (xrP_BOOL = BOOL)
                    skip_marker(*chunk);                    // "Base texture"
                    skip_prop(*chunk, sizeof(string64));     // oT_Name
                    skip_prop(*chunk, sizeof(string64));     // oT_xform
                    skip_marker(*chunk);                    // "Detail map"
                    skip_prop(*chunk, sizeof(string64));     // oT2_Name
                    skip_prop(*chunk, sizeof(string64));     // oT2_xform
                    read_str64(*chunk, R);                  // oR_Name
                    read_str64(*chunk, G);                  // oG_Name
                    read_str64(*chunk, B);                  // oB_Name
                    read_str64(*chunk, A);                  // oA_Name

                    std::string key(desc.cName);
                    for (char& c : key) c = (char)tolower((unsigned char)c);
                    s_BlenderDet[key] = { std::string(R), std::string(G), std::string(B), std::string(A) };
                    ++bmmd;
                }
                chunk->close();
                ++cid;
            }
            fs->close();
        }
        FS.r_close(F);

        // Variety summary: how many DISTINCT detail-sets across all terrain shaders.
        std::set<std::string> distinct;
        for (auto& kv : s_BlenderDet)
            distinct.insert(kv.second[0] + "|" + kv.second[1] + "|" + kv.second[2] + "|" + kv.second[3]);
        Msg("[VK Terrain] shaders.xr: %d blenders, %d B_BmmD terrain shaders, %u DISTINCT detail-sets",
            total, bmmd, (u32)distinct.size());
        int n = 0;
        for (const std::string& s : distinct) { Msg("[VK Terrain]   set %d: %s", n++, s.c_str()); }
    }

    // Clear the one-shot latch and try again. Safe to call at any point BEFORE the
    // first terrain material is built (the host sets its root during boot, terrain
    // materials appear at scene push, seconds later), and a no-op once the map has
    // actually loaded — a successful load leaves s_BlenderDet non-empty.
    void RetryTerrainBlenderMapImpl()
    {
        if (!s_BlenderDet.empty()) return;   // already loaded for real
        s_BlenderDetLoaded = false;
        LoadTerrainBlenderMap();
    }

    // Write the 15-binding terrain set: base, mask, dt_r..dt_a, lmap, dn_r..dn_a,
    // dh_r..dh_a (the 4 <detail>_height maps for SSFX-style terrain POM).
    void WriteTerrainSet(VkDescriptorSet set, const VkImageView v[15])
    {
        VK::DescriptorWriter w(set);
        for (u32 i = 0; i < 15; ++i) w.ImageSampler(i, v[i], s_Sampler);
        w.Flush();
    }

    WorldMaterial* CreateDefaultWhite()
    {
        // 1×1 white as the universal fallback for missing textures.
        const u8 white[4] = { 255, 255, 255, 255 };
        auto* tex = xr_new<CVulkanTexture>();
        tex->CreateFromData(white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);

        auto* m = xr_new<WorldMaterial>();
        m->tex          = tex;
        m->view         = tex->GetView();
        m->sampler      = s_Sampler;
        m->view_detail  = s_GreyDetail->GetView();   // 0.5 detail → no-op
        m->detailScale  = 0.0f;
        m->view_lmap    = s_WhiteLmap->GetView();    // 1.0 lmap   → no-op
        m->alphaRef     = -1.0f;
        m->set          = AllocateSet();
        if (m->set != VK_NULL_HANDLE)
            WriteSet(m->set, m->view, m->view_detail, m->view_lmap, s_FlatBump->GetView(), s_FlatBumpN->GetView());
        return m;
    }
}

VkDescriptorSetLayout GetSetLayout()        { return s_SetLayout;        }
// r_world_bindless: the five views WriteSet puts in this material's set (base, detail, lmap, bump, bump normal), with the
// same fallbacks -- the bindless slots must sample exactly what the per-material set does.
void SetViews(const WorldMaterial* m, VkImageView out[5])
{
    const WorldMaterial* d = s_Default;
    out[0] = m->view        ? m->view        : (d ? d->view : VK_NULL_HANDLE);
    out[1] = m->view_detail ? m->view_detail : (d ? d->view_detail : VK_NULL_HANDLE);
    out[2] = m->view_lmap   ? m->view_lmap   : (d ? d->view_lmap : VK_NULL_HANDLE);
    out[3] = m->view_bump   ? m->view_bump   : (s_FlatBump  ? s_FlatBump->GetView()  : VK_NULL_HANDLE);
    out[4] = m->view_bumpn  ? m->view_bumpn  : (s_FlatBumpN ? s_FlatBumpN->GetView() : VK_NULL_HANDLE);
}
VkDescriptorSetLayout GetTerrainSetLayout() { return s_TerrainSetLayout; }
VkSampler             GetSampler()          { return s_Sampler;          }
WorldMaterial*        GetDefault()          { return s_Default;          }

// Rewrite binding 1 (splat mask) of every mask-less terrain material to the
// TerrainMask bake. Level-load-end only: nothing in flight references the sets.
void RebindTerrainMasks(VkImageView view)
{
    if (view == VK_NULL_HANDLE) return;
    u32 n = 0;
    for (auto& kv : s_Cache) {
        WorldMaterial* m = kv.second;
        if (!m || !m->isTerrain || m->terrainChannel == 255 || m->terrainSet == VK_NULL_HANDLE) continue;
        VK::DescriptorWriter(m->terrainSet).ImageSampler(1, view, s_Sampler).Flush();
        ++n;
    }
    Msg("[VK Terrain] baked splat mask bound to %u mask-less terrain material(s)", n);
}

// Recycle retired sets past the fence horizon (FRAMES_IN_FLIGHT=3, +1 margin).
// Runs right after the frame fence in CRender::Begin — a set retired at frame N
// was last bound by frame N-1's recording at the latest; by N+4 every submission
// that could reference it has fenced out and it may be rewritten/reused.
void FrameTick()
{
    std::lock_guard<std::mutex> lk(s_StreamMutex);
    if (s_RetiredSets.empty()) return;
    const u32 now = RV().frameIndex;
    size_t w = 0;
    for (size_t i = 0; i < s_RetiredSets.size(); ++i) {
        RetiredSet& e = s_RetiredSets[i];
        if (now < e.frame /*counter reset*/ || now - e.frame > 4)
            s_FreeSets.push_back(e.set);
        else
            s_RetiredSets[w++] = e;
    }
    s_RetiredSets.resize(w);
}

// Streamer rebind callback: a WorldDiffuse texture `t` just changed its image (mip
// promote/demote). NEVER rewrite the live set — in-flight frames still sample
// through it. Each affected material gets a FRESH set (free list first) with all
// four bindings written; the old set retires and is recycled by FrameTick after
// the fence horizon. Runs in CRender::Begin (fence waited, nothing recording), so
// the flipped `m->set` is what this frame's recording picks up.
//
// Terrain materials never come through here: their base diffuse is opted out of
// streaming at GetOrCreate (vk_terrain_cache captures the terrain set by handle —
// a retired handle there would dangle).
void RebindStreamedTexture(CVulkanTexture* t)
{
    if (!t) return;
    const VkImageView nv = t->GetView();
    for (auto& kv : s_Cache) {
        WorldMaterial* m = kv.second;
        if (!m) continue;
        // `t` is either this material's base diffuse or its `<bump>` normal — both
        // classes are mip-streamed. A bump is shared by name across many materials,
        // so this loop legitimately rewrites several sets for one swap.
        const bool isBase = (m->tex       == t);
        const bool isBumpN = (m->tex_bumpn == t);
        if (!isBase && !isBumpN) continue;
        if (isBase)  m->view       = nv;
        if (isBumpN) m->view_bumpn = nv;

        VkDescriptorSet ns = AllocateSet();
        if (ns == VK_NULL_HANDLE) continue;   // pool exhausted — keep the old set/view pair
        WriteSet(ns, m->view, m->view_detail, m->view_lmap,
                 m->view_bump  != VK_NULL_HANDLE ? m->view_bump  : s_FlatBump->GetView(),
                 m->view_bumpn != VK_NULL_HANDLE ? m->view_bumpn : s_FlatBumpN->GetView());

        VkDescriptorSet old = m->set;
        m->set = ns;
        if (old != VK_NULL_HANDLE) {
            std::lock_guard<std::mutex> lk(s_StreamMutex);
            s_RetiredSets.push_back({ RV().frameIndex, old });
        }
    }
}

bool Init()
{
    if (s_SetLayout) return true;

    // Set 0: { binding 0 = diffuse, binding 1 = detail, binding 2 = lmap,
    // binding 3 = bump# height }, all combined image samplers. Bindings 0-2
    // are fragment-only; binding 3 is sampled by the tessellation evaluation
    // shader (displacement height in its alpha). Four bindings per set
    // requires the pool descriptorCount to be 4× maxSets.
    // Binding 4 = `<bump>.dds`: the material's tangent NORMAL (R4 packing: n =
    // tex.wzy*2-1) plus its GLOSS in .x. Statics had no normal map at all in this
    // forward path — wall relief came only from the `#` height via POM — and no
    // per-material gloss, so the sky specular ran on one invented roughness for the
    // whole world (see world_lmap_frag_body). Same texture answers both.
    VkDescriptorSetLayoutBinding b[5]{};
    for (int i = 0; i < 5; ++i) {
        b[i].binding         = (u32)i;
        b[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].descriptorCount = 1;
        b[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (VulkanHW.m_bTessellationSupported)
        b[3].stageFlags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 5;
    lci.pBindings    = b;
    if (vkCreateDescriptorSetLayout(VulkanHW.m_Device, &lci, nullptr, &s_SetLayout) != VK_SUCCESS) {
        Msg("![VK WorldMaterial] CreateDescriptorSetLayout failed");
        return false;
    }

    // The material cache is NAME-keyed and persists for the whole device
    // lifetime — it is NOT reset per level, because persistent visuals (the
    // actor's HUD/weapon, carried items) survive a location change and keep a
    // raw WorldMaterial* into this cache. So sets accumulate across every
    // location loaded this session: ~549 unique materials per level in test
    // logs. Sized for ~30 distinct levels so a full playthrough (revisits are
    // cache hits) never exhausts the pool — exhaustion previously cascaded into
    // VK_ERROR_OUT_OF_POOL_MEMORY → default-fallback churn → a multi-second
    // frame → TDR → DEVICE_LOST on the 2nd–3rd transition. A set is 4 image
    // samplers; 16384 sets is a few MB of pool — cheap insurance.
    // TODO: the leak-free fix is ref-counting materials (free when the last
    // visual referencing one is destroyed) so the pool can actually shrink.
    constexpr u32 kMaxSets = 16384;
    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = kMaxSets * 5;   // 5 image samplers per set (base/detail/lmap/bump#/bump)

    VkDescriptorPoolCreateInfo pci{};
    pci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets       = kMaxSets;
    pci.poolSizeCount = 1;
    pci.pPoolSizes    = &ps;
    if (vkCreateDescriptorPool(VulkanHW.m_Device, &pci, nullptr, &s_Pool) != VK_SUCCESS) {
        Msg("![VK WorldMaterial] CreateDescriptorPool failed");
        return false;
    }

    // Shared sampler. samplerAnisotropy is enabled at device creation
    // (HW_Vulkan.cpp), so anisotropic filtering is safe to request here.
    // Without it, terrain at glancing angles trilinear-collapses to coarse
    // mips → smudgy "gouache paint" look on the ground.
    VkSamplerCreateInfo si{};
    si.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter        = VK_FILTER_LINEAR;
    si.minFilter        = VK_FILTER_LINEAR;
    si.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.minLod           = 0.0f;
    si.maxLod           = VK_LOD_CLAMP_NONE;
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy    = 16.0f;   // device limit is queried but every modern GPU clamps to ≥16
#ifdef __SWITCH__
    si.anisotropyEnable = VK_FALSE; // 16x aniso costs a lot of texel bandwidth on GM20B
    si.maxAnisotropy = 1.0f;
#endif
    if (vkCreateSampler(VulkanHW.m_Device, &si, nullptr, &s_Sampler) != VK_SUCCESS) {
        Msg("![VK WorldMaterial] CreateSampler failed");
        return false;
    }

    // Grey 0.5 detail fallback. With the formula `2 * base * detail`, a
    // material with no .thm-declared detail samples 0.5 → factor 1.0 → no-op.
    {
        const u8 grey[4] = { 128, 128, 128, 255 };
        s_GreyDetail = xr_new<CVulkanTexture>();
        s_GreyDetail->CreateFromData(grey, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);
    }

    // White lmap fallback for vert-lit / non-lightmapped materials. Frag
    // shader does `albedo *= lm`, so 1.0 is a no-op. The vert-lit shader
    // variant doesn't sample binding 2 anyway — the white texture only
    // satisfies the descriptor's validity requirement.
    {
        const u8 white[4] = { 255, 255, 255, 255 };
        s_WhiteLmap = xr_new<CVulkanTexture>();
        s_WhiteLmap->CreateFromData(white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);
    }

    // Flat bump# fallback for materials with no `<bump>#` height. ALPHA = 255
    // (height = 1 = top surface): POM depth = 1-h = 0 → the parallax march
    // exits immediately → NO parallax on these materials (auto-gate). Tess is
    // unaffected — only real-bump materials enter the tess pipeline, so a TES
    // never samples this fallback (and the planned high-pass makes a constant
    // height a no-op there regardless).
    {
        const u8 flat[4] = { 128, 128, 128, 255 };
        s_FlatBump = xr_new<CVulkanTexture>();
        s_FlatBump->CreateFromData(flat, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);
    }

    // Flat normal + NEUTRAL gloss fallback for binding 4. The R4 unpack is
    // n = tex.wzy*2-1, so a flat (0,0,1) tangent normal needs w=0.5, z=0.5, y=1.0.
    //
    // ⚠ The gloss byte is picked from MEASURED CONTENT, and the obvious-looking choice
    // is the wrong one. Every <bump>.dds in this install was probed (118 files, gloss =
    // the R channel): p50 = 0.031, p90 = 0.094, p95 = 0.161, p99 = 0.322. X-Ray authored
    // these for R2's much weaker specular, so real gloss lives in the bottom sixth of
    // the range and 90% of the world lands in roughness 0.78-0.85.
    //
    // The first attempt here was 102 (0.4), reasoned as "reproduce the pre-feature flat
    // roughness 0.55 so materials without data keep their old look". That is coherent
    // with HISTORY and incoherent with NEIGHBOURS, which is what the eye actually reads:
    // 0.4 sits at the p99.7 of the content, so a material with no bump came out glossier
    // than 99.7% of the materials that have one. User-visible as two adjacent fences,
    // the one WITHOUT data looking lacquered next to the one with it.
    //
    // 13 (0.051) sits between the content's p50 and p75 — a no-data material now shades
    // like a typical material that has data. Re-measure before changing this: the right
    // value is a property of the installed textures, not a taste constant.
    {
        const u8 flatN[4] = { 13, 255, 128, 128 };    // r = gloss 0.051 ~ content median (p50 0.031 / p75 0.063)
        s_FlatBumpN = xr_new<CVulkanTexture>();
        s_FlatBumpN->CreateFromData(flatN, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);
    }

    s_Default = CreateDefaultWhite();

    // ----- Terrain splatting set layout + pool + default channel details -----
    {
        // 15 combined image samplers, fragment-only: base, mask, dt_r..dt_a,
        // lmap, dn_r..dn_a (the 4 <detail>_bump tangent normal maps), dh_r..dh_a
        // (the 4 <detail>_height maps for SSFX-style terrain parallax-occlusion).
        VkDescriptorSetLayoutBinding tb[15]{};
        for (int i = 0; i < 15; ++i) {
            tb[i].binding         = (u32)i;
            tb[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[i].descriptorCount = 1;
            // + COMPUTE: vk_terrain_cache binds this same set to its bake pipeline
            // (reads mask + the 4 heights) — one flag here covers every pooled set.
            tb[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        // uMask (1) also feeds the terrain TESS EVAL: the mud footprint geometric
        // carve reads the splat softness so asphalt never dents.
        if (VulkanHW.m_bTessellationSupported)
            tb[1].stageFlags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        VkDescriptorSetLayoutCreateInfo tlci{};
        tlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        tlci.bindingCount = 15;
        tlci.pBindings    = tb;
        vkCreateDescriptorSetLayout(VulkanHW.m_Device, &tlci, nullptr, &s_TerrainSetLayout);

        // Terrain materials are few per level (a handful of ground textures),
        // but — like the diffuse pool above — they accumulate across every
        // location this session (cache is never reset). 4096 covers a full
        // multi-level playthrough without exhausting this pool either.
        constexpr u32 kMaxTerrain = 4096;
        VkDescriptorPoolSize tps{};
        tps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tps.descriptorCount = kMaxTerrain * 15;
        VkDescriptorPoolCreateInfo tpci{};
        tpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        tpci.maxSets       = kMaxTerrain;
        tpci.poolSizeCount = 1;
        tpci.pPoolSizes    = &tps;
        vkCreateDescriptorPool(VulkanHW.m_Device, &tpci, nullptr, &s_TerrainPool);

        // 1×1 white mask fallback (normalized → even blend; never black).
        const u8 white[4] = { 255, 255, 255, 255 };
        s_WhiteMask = xr_new<CVulkanTexture>();
        s_WhiteMask->CreateFromData(white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);

        // One-hot channel masks for shader-regionalized (mask-less) terrain.
        for (int c = 0; c < 4; ++c) {
            u8 onehot[4] = { 0, 0, 0, 0 };
            onehot[c] = 255;
            s_ChannelMask[c] = xr_new<CVulkanTexture>();
            s_ChannelMask[c]->CreateFromData(onehot, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);
        }

        // Flat tangent-normal fallback (0,0,1). R4 decodes a detail-normal as
        // `n = tex.wzy*2-1` (gloss in R; tangent normal packed in A,B,G). For a
        // no-op normal we need A=0.5, B=0.5, G=1.0 → RGBA {0,255,128,128}.
        const u8 flatN[4] = { 0, 255, 128, 128 };
        s_FlatNormal = xr_new<CVulkanTexture>();
        s_FlatNormal->CreateFromData(flatN, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);

        // Flat height fallback (mid-grey .r=0.5). The POM march reads a UNIFORM
        // height -> zero parallax for any channel whose `_height` map is missing
        // (graceful on packs that ship details without height maps).
        const u8 flatH[4] = { 128, 128, 128, 255 };
        s_FlatHeight = xr_new<CVulkanTexture>();
        s_FlatHeight->CreateFromData(flatH, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4);

        // Default channel-detail textures (CBlender_BmmD defaults). Per-shader
        // overrides live in shaders.xr; these cover the common case.
        static const char* kDet[4] = {
            "detail\\detail_grnd_grass",   // R
            "detail\\detail_grnd_asphalt", // G
            "detail\\detail_grnd_earth",   // B
            "detail\\detail_grnd_yantar",  // A
        };
        for (int i = 0; i < 4; ++i) {
            s_TerrainDetail[i] = GetOrLoadGameTex(s_TerrainDetCache, kDet[i], s_GreyDetail);
            // CBlender_BmmD names the detail-normal as "<detail>_bump". Missing
            // ones fall back to the flat normal (no perturbation) — graceful on
            // modpacks that ship diffuse details without bumps.
            std::string bn = std::string(kDet[i]) + "_bump";
            s_TerrainNormal[i] = GetOrLoadGameTex(s_TerrainDetCache, bn.c_str(), s_FlatNormal);
            Msg("[VK Terrain] detail-normal '%s' -> %s", bn.c_str(),
                (s_TerrainNormal[i] != s_FlatNormal) ? "loaded" : "MISSING (flat)");

            // <detail>_height for SSFX-style terrain POM. Missing -> flat (no relief).
            std::string hn = std::string(kDet[i]) + "_height";
            s_TerrainHeight[i] = GetOrLoadGameTex(s_TerrainDetCache, hn.c_str(), s_FlatHeight);
            Msg("[VK Terrain] detail-height '%s' -> %s", hn.c_str(),
                (s_TerrainHeight[i] != s_FlatHeight) ? "loaded" : "MISSING (flat)");
        }

        // Probe shaders.xr for per-shader detail sets (diagnostic for now; the
        // per-material lookup that consumes s_BlenderDet is the next step).
        LoadTerrainBlenderMap();
    }

    // Let the texture streamer rewrite our base-diffuse descriptors when it swaps a
    // WorldDiffuse texture's mip residency (dynamic streaming, r_txstream).
    VK::TextureStreamer::Instance().SetRebindCallback(&RebindStreamedTexture);

    Msg("[VK WorldMaterial] Init OK (pool=%u sets base+detail+lmap+bump#; terrain pool x15 w/ detail-normals + detail-heights; anisotropic 16x)", kMaxSets);
    return true;
}

void Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;

    for (auto& kv : s_Cache) {
        if (!kv.second) continue;
        if (kv.second == s_Default) continue;  // s_Cache may alias s_Default for failed lookups
        // NB: `tex` is NOT freed here — base diffuses are shared by resolved path
        // across material keys (s_DiffuseTexCache owns them, freed once below).
        // Deleting per material would double-free every shared one.
        xr_delete(kv.second);
    }
    s_Cache.clear();

    // Base diffuses are shared (one entry per unique .dds path); free once.
    for (auto& kv : s_DiffuseTexCache) {
        if (kv.second) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_DiffuseTexCache.clear();

    // Detail textures are shared (one entry per unique detail name); free once.
    for (auto& kv : s_DetailTexCache) {
        if (kv.second && kv.second != s_GreyDetail) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_DetailTexCache.clear();

    if (s_GreyDetail) {
        s_GreyDetail->Destroy();
        xr_delete(s_GreyDetail);
        s_GreyDetail = nullptr;
    }

    for (auto& kv : s_LmapTexCache) {
        if (kv.second && kv.second != s_WhiteLmap) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_LmapTexCache.clear();

    if (s_WhiteLmap) {
        s_WhiteLmap->Destroy();
        xr_delete(s_WhiteLmap);
        s_WhiteLmap = nullptr;
    }

    // Bump# height textures (tessellation displacement), shared by reference.
    for (auto& kv : s_BumpTexCache) {
        if (kv.second && kv.second != s_FlatBump) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_BumpTexCache.clear();

    if (s_FlatBump) {
        s_FlatBump->Destroy();
        xr_delete(s_FlatBump);
        s_FlatBump = nullptr;
    }

    // Material normal+gloss textures (binding 4), shared by reference.
    for (auto& kv : s_BumpNTexCache) {
        if (kv.second && kv.second != s_FlatBumpN) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_BumpNTexCache.clear();

    if (s_FlatBumpN) {
        s_FlatBumpN->Destroy();
        xr_delete(s_FlatBumpN);
        s_FlatBumpN = nullptr;
    }

    // Terrain splat resources. s_TerrainDetail[] alias entries in
    // s_TerrainDetCache (or the grey/white fallbacks) — free the cache once,
    // skipping shared fallbacks.
    for (auto& kv : s_TerrainDetCache) {
        if (kv.second && kv.second != s_GreyDetail && kv.second != s_WhiteMask
            && kv.second != s_FlatNormal && kv.second != s_FlatHeight) {
            kv.second->Destroy();
            xr_delete(kv.second);
        }
    }
    s_TerrainDetCache.clear();
    for (int i = 0; i < 4; ++i) { s_TerrainDetail[i] = nullptr; s_TerrainNormal[i] = nullptr; s_TerrainHeight[i] = nullptr; }

    if (s_FlatNormal) {
        s_FlatNormal->Destroy();
        xr_delete(s_FlatNormal);
        s_FlatNormal = nullptr;
    }
    if (s_FlatHeight) {
        s_FlatHeight->Destroy();
        xr_delete(s_FlatHeight);
        s_FlatHeight = nullptr;
    }

    if (s_WhiteMask) {
        s_WhiteMask->Destroy();
        xr_delete(s_WhiteMask);
        s_WhiteMask = nullptr;
    }
    for (int c = 0; c < 4; ++c) {
        if (s_ChannelMask[c]) {
            s_ChannelMask[c]->Destroy();
            xr_delete(s_ChannelMask[c]);
            s_ChannelMask[c] = nullptr;
        }
    }
    if (s_TerrainPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(VulkanHW.m_Device, s_TerrainPool, nullptr);
        s_TerrainPool = VK_NULL_HANDLE;
    }
    if (s_TerrainSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_TerrainSetLayout, nullptr);
        s_TerrainSetLayout = VK_NULL_HANDLE;
    }

    if (s_Default) {
        if (s_Default->tex) { s_Default->tex->Destroy(); xr_delete(s_Default->tex); }
        xr_delete(s_Default);
        s_Default = nullptr;
    }

    {   // Retired/free sets belong to s_Pool — the pool destroy below frees them.
        std::lock_guard<std::mutex> lk(s_StreamMutex);
        s_RetiredSets.clear();
        s_FreeSets.clear();
    }
    if (s_Pool) {
        // Frees all sets allocated from the pool — including default + cached.
        vkDestroyDescriptorPool(VulkanHW.m_Device, s_Pool, nullptr);
        s_Pool = VK_NULL_HANDLE;
    }
    if (s_Sampler) {
        vkDestroySampler(VulkanHW.m_Device, s_Sampler, nullptr);
        s_Sampler = VK_NULL_HANDLE;
    }
    if (s_SetLayout) {
        vkDestroyDescriptorSetLayout(VulkanHW.m_Device, s_SetLayout, nullptr);
        s_SetLayout = VK_NULL_HANDLE;
    }
}

void SetLevelTag(const char* tag)
{
    {   // level-local .thm files make the cache level-scoped
        std::lock_guard<std::mutex> lk(s_thmMx);
        s_thmCache.clear();
    }
    s_thmCalls = s_thmHits = 0;
    s_thmClk = 0;
    s_LevelTag = (tag && tag[0]) ? tag : "";
    Msg("[VK WorldMaterial] level tag: '%s'", s_LevelTag.c_str());
}

// ---------------------------------------------------------------------------
// Stage 0 profiling. LoadTexture owns 6721 of the 7447 ms visuals phase, and
// this call is the only non-trivial thing it does per visual - so split the
// call itself: the terrain mask probe runs BEFORE the cache lookup and hits the
// filesystem, which would make the cache useless for terrain materials. Numbers
// decide, not the reading.
// ---------------------------------------------------------------------------
namespace {
// In ticks: 388k calls a load, and the two inner scopes fire almost as often.
// A microsecond-truncating timer loses half a microsecond per sample, which at
// this count is hundreds of milliseconds reported as someone else's cost.
u64   s_profTotalClk = 0, s_profProbeClk = 0, s_profFindClk = 0;
u32   s_profCalls = 0, s_profHits = 0, s_profTerrain = 0;

using prof_scope = VK::ClkScope;
}

// ---------------------------------------------------------------------------
// Parallel texture prefetch (see the header). Two halves: `ExpandDiffusePaths`
// turns one shader-table base into the files that base pulls in, and
// `StartTexturePrefetch` hands the whole table to the worker pool.
//
// The resolution rules below MIRROR the loaders further down this file — that
// duplication is deliberate and cheap to be wrong about: this side decides only
// WHICH FILES TO OPEN EARLY. A path that no loader ever asks for is one wasted
// read; a path this misses just loads the old way. Nothing here can change what
// the level ends up with.
// ---------------------------------------------------------------------------
namespace {

// $game_textures$ then $level$ — the GetOrLoadGameTex rule (bump, height, mask).
// The KEY that cache uses is the name, not the path, so the job carries both:
// a worker publishing a finished texture must file it where the walk will look.
void PushGameTexJob(const char* name, TexRole role, xr_vector<TexJob>& out)
{
    if (!name || !name[0]) return;
    string_path leaf, p;
    xr_sprintf(leaf, "%s.dds", name);
    FS.update_path(p, "$game_textures$", leaf);
    if (!FS.exist(p)) {
        FS.update_path(p, "$level$", leaf);
        if (!FS.exist(p)) return;
    }
    out.push_back(TexJob{ std::string(p), std::string(name), role });
}

void ExpandDiffuseJobs(const char* diffuse_name, xr_vector<TexJob>& out)
{
    if (!diffuse_name || !diffuse_name[0]) return;

    string_path full;
    // s_DiffuseTexCache is keyed by the RESOLVED path (several materials share one
    // diffuse), so here the key and the path are the same string.
    if (ResolveTexturePath(diffuse_name, full))
        out.push_back(TexJob{ std::string(full), std::string(full), TexRole::Diffuse });

    // Everything else a material pulls in is declared by the diffuse's .thm.
    THMInfo thm{};
    if (LookupTHM(diffuse_name, thm)) {
        if (thm.has_detail && !thm.detail_name.empty()) {
            // GetOrLoadDetailTex looks in $game_textures$ and nowhere else, and keys
            // on the bare detail name.
            string_path leaf, p;
            xr_sprintf(leaf, "%s.dds", thm.detail_name.c_str());
            FS.update_path(p, "$game_textures$", leaf);
            if (FS.exist(p))
                out.push_back(TexJob{ std::string(p), thm.detail_name, TexRole::Detail });
        }
        if (thm.has_bump && !thm.bump_name.empty()) {
            // Normal+gloss is behind r_bump — that cvar is a MEMORY gate, not just
            // a shading one (GetOrCreate skips the load entirely at 0), so honour it
            // here too instead of reading 563 MB nothing will ask for.
            if (ps_r_bump > 0.0f) PushGameTexJob(thm.bump_name.c_str(), TexRole::Bump, out);
            std::string height = thm.bump_name + "#";
            if (HeightTexNeeded()) PushGameTexJob(height.c_str(), TexRole::Height, out);
        }
    }

    // Terrain splat mask (`<diffuse>_mask`) — mask-less maps fall back to a
    // synthesized one-hot, and then this simply resolves to nothing.
    if (strstr(diffuse_name, "terrain\\") == diffuse_name ||
        strstr(diffuse_name, "terrain/")  == diffuse_name) {
        string_path mask_name;
        xr_sprintf(mask_name, "%s_mask", diffuse_name);
        PushGameTexJob(mask_name, TexRole::Mask, out);
    }
}

// Finish one job on a prefetch worker: build the image and file it in the cache the
// walk will look in, so the walk's own call finds it instead of doing all of this on
// the thread the level is waiting for. The per-role parameters MUST match the loader
// that owns the cache (right above) — a mismatch would hand the walk a texture in the
// wrong colour space or stream class, which is why they sit next to the resolution
// rules they mirror.
void MaterializeJob(const TexJob& job)
{
    switch (job.role) {
    case TexRole::Diffuse:
        LoadIntoCache(s_DiffuseTexCache, job.key, job.path.c_str(), nullptr,
                      TexStreamClass::WorldDiffuse, TexColorSpace::Color, false);
        break;
    case TexRole::Detail:
        LoadIntoCache(s_DetailTexCache, job.key, job.path.c_str(), s_GreyDetail,
                      TexStreamClass::Detail, TexColorSpace::Data, false);
        break;
    case TexRole::Bump:
        LoadIntoCache(s_BumpNTexCache, job.key, job.path.c_str(), s_FlatBumpN,
                      TexStreamClass::Bump, TexColorSpace::Data, false);
        break;
    case TexRole::Height:
        LoadIntoCache(s_BumpTexCache, job.key, job.path.c_str(), s_FlatBump,
                      TexStreamClass::Terrain, TexColorSpace::Data, /*alphaOnly*/ true);
        break;
    case TexRole::Mask:
        LoadIntoCache(s_TerrainDetCache, job.key, job.path.c_str(), s_WhiteMask,
                      TexStreamClass::Terrain, TexColorSpace::Data, false);
        break;
    case TexRole::Lmap:
        LoadIntoCache(s_LmapTexCache, job.key, job.path.c_str(), s_WhiteLmap,
                      TexStreamClass::Lmap, TexColorSpace::Data, false);
        break;
    }
}
}   // anonymous namespace

void StartTexturePrefetch(xr_vector<shared_str>&& diffuseNames, xr_vector<shared_str>&& lmapNames,
                          const char* leadFile)
{
    // The shader table repeats a diffuse across regions (pripyat_asfalt/earth/grass
    // share one base), and expanding it twice would read its .thm twice.
    std::unordered_set<std::string> seen;
    xr_vector<shared_str> bases;
    bases.reserve(diffuseNames.size());
    u32 alreadyLoaded = 0;
    for (const shared_str& n : diffuseNames) {
        if (n.size() == 0) continue;
        if (!seen.insert(std::string(n.c_str())).second) continue;
        // This cache deliberately SURVIVES a level change (persistent visuals hold
        // raw WorldMaterial*), so re-entering a level the session already visited
        // means GetOrCreate hits everything and opens no file at all. Measured on a
        // second load: 1114 files / 1025 MB prefetched, zero claimed. Filtering here
        // and not in the worker on purpose — the caches belong to the loading thread,
        // and at this point in the load nothing is touching them yet.
        string_path full;
        if (ResolveTexturePath(n.c_str(), full) &&
            FindInCache(s_DiffuseTexCache, std::string(full)) != nullptr) {
            ++alreadyLoaded;
            continue;
        }
        bases.push_back(n);
    }
    if (alreadyLoaded)
        Msg("[VK TexPrefetch] %u of %u shader bases already resident from an earlier level — not prefetched",
            alreadyLoaded, (u32)seen.size());


    // Lightmaps resolve $level$-first (GetOrLoadLmapTex) — the opposite order from
    // everything else, because an lmap name is level-local by construction.
    std::unordered_set<std::string> seenLmap;
    xr_vector<TexJob> direct;
    direct.reserve(lmapNames.size());
    for (const shared_str& n : lmapNames) {
        if (n.size() == 0) continue;
        if (!seenLmap.insert(std::string(n.c_str())).second) continue;
        // Same "already resident" skip as the bases above. Lightmap keys carry the
        // level tag, so this only ever matches a level visited before. The key is
        // kept: it is what GetOrLoadLmapTex looks under, so a worker needs it to file
        // the finished texture there.
        std::string key(s_LevelTag);
        key.push_back('|');
        key.append(n.c_str());
        if (FindInCache(s_LmapTexCache, key) != nullptr) continue;

        string_path leaf, p;
        xr_sprintf(leaf, "%s.dds", n.c_str());
        FS.update_path(p, "$level$", leaf);
        if (!FS.exist(p)) {
            FS.update_path(p, "$game_textures$", leaf);
            if (!FS.exist(p)) continue;
        }
        direct.push_back(TexJob{ std::string(p), std::move(key), TexRole::Lmap });
    }

    VK::TexPrefetch::Start(std::move(bases), std::move(direct), &ExpandDiffuseJobs,
                           &MaterializeJob, leadFile);
}

void ProfDump()
{
    const float k = VK::ClkToMs();
    Msg("[load step]   LookupTHM: %u calls (%u cache hits) | %.0f ms of wall across every caller",
        s_thmCalls.load(), s_thmHits.load(), k * float(s_thmClk.load()));
    // Who built the level's textures, and what the walk paid to let workers do it:
    // a wait here is the walk arriving at a file a worker had already started.
    Msg("[load step]   texture caches: %u built | %u waits for another thread (%.0f ms)",
        s_texBuilt.load(), s_texWaits.load(), k * float(s_texWaitClk.load()));
    Msg("[load step]   GetOrCreate miss split: diffuse %.0f | terrain heal %.0f | thm+detail %.0f | bumpX %.0f | bumpN %.0f | lmap %.0f | diag Msg %.0f | material+set %.0f ms",
        k * float(s_mkDiffuseClk.load()), k * float(s_mkTerrainClk.load()), k * float(s_mkThmClk.load()),
        k * float(s_mkBumpxClk.load()), k * float(s_mkBumpnClk.load()), k * float(s_mkLmapClk.load()),
        k * float(s_mkDiagClk.load()), k * float(s_mkSetClk.load()));
    s_texBuilt = 0; s_texWaits = 0; s_texWaitClk = 0;
    s_mkDiffuseClk = s_mkTerrainClk = s_mkThmClk = s_mkBumpxClk = 0;
    s_mkBumpnClk = s_mkLmapClk = s_mkDiagClk = s_mkSetClk = 0;
    Msg("[load step]   GetOrCreate: %u calls (%u hits, %u terrain-probed) | total %.0f ms = probe %.0f + find %.0f + create %.0f",
        s_profCalls, s_profHits, s_profTerrain, k * float(s_profTotalClk), k * float(s_profProbeClk),
        k * float(s_profFindClk), k * float(s_profTotalClk - s_profProbeClk - s_profFindClk));
    s_profTotalClk = s_profProbeClk = s_profFindClk = 0;
    s_profCalls = s_profHits = s_profTerrain = 0;
}

WorldMaterial* GetOrCreate(const char* diffuse_name, const char* lmap_name, float alphaRef, bool wmark,
                           const char* shader_name)
{
    if (!s_SetLayout || !s_Default) return s_Default;
    if (!diffuse_name || !diffuse_name[0]) return s_Default;

    prof_scope _profAll(s_profTotalClk);
    ++s_profCalls;

    const bool is_terrain = (strstr(diffuse_name, "terrain\\") == diffuse_name ||
                             strstr(diffuse_name, "terrain/")  == diffuse_name);

    // Mask-less terrain? (Decides the cache key below, so probed up front.) A map
    // WITH a real `_mask` (Cordon/Bar) keeps the exact pre-existing behavior —
    // one material, real mask, no shader keying.
    bool terrain_maskless = false;
    if (is_terrain) {
        prof_scope _profProbe(s_profProbeClk);
        ++s_profTerrain;
        string_path mask_leaf, mask_full;
        xr_sprintf(mask_leaf, "%s_mask", diffuse_name);
        terrain_maskless = !ResolveTexturePath(mask_leaf, mask_full);
    }

    // Same diffuse can pair with different lmaps — key on both. Lmap-bearing
    // materials are ALSO namespaced by the level tag: lmap names repeat across
    // levels with different content, and the cache survives level changes (a
    // stale entry would keep the previous level's lightmap in its descriptor
    // set — the "baked patches after a level transition" bug). Lmap-less
    // materials (weapons/NPC/props) stay global so persistent visuals keep
    // hitting their entries.
    // MASK-LESS terrain is additionally keyed by the LEVEL SHADER: there the
    // shader IS the region (pripyat_asfalt/earth/grass share one diffuse+lmap
    // but must get different synthesized splat masks — one key would collapse
    // them into a single material).
    const u64 _profFindC0 = CPU::GetCLK();

    std::string key(diffuse_name);
    key.push_back('|');
    if (lmap_name && lmap_name[0]) { key.append(lmap_name); key.push_back('|'); key.append(s_LevelTag); }
    if (terrain_maskless && shader_name && shader_name[0]) { key.push_back('|'); key.append(shader_name); }

    auto it = s_Cache.find(key);
    s_profFindClk += CPU::GetCLK() - _profFindC0;

    if (it != s_Cache.end()) {
        ++s_profHits;
        // Materials are shared by texture pair — if ANY user is a wallmark
        // shader, the whole material renders as a decal (textures are
        // decal-dedicated in practice). Same upgrade for glass (aref == -2).
        if (wmark && it->second != s_Default) {
            it->second->isWmark = true;
            if (alphaRef < -4.5f)      it->second->isWater    = true;
            else if (alphaRef < -3.5f) it->second->isLitBlend = true;
            else if (alphaRef < -2.5f) it->second->isEmisAdd  = true;
            else if (alphaRef < -1.5f) it->second->isGlass    = true;
        }
        return it->second;
    }

    string_path full;
    if (!ResolveTexturePath(diffuse_name, full)) {
        // A world/terrain material whose base .dds can't be resolved silently
        // fell back to the 1x1 white default — that is exactly how a level ends
        // up with an all-white "no terrain" ground (e.g. terrain\terrain_bolota
        // on Garbage: its .thm ships but the bitmap is absent from the textures
        // archive). Log it once per unique name so the culprit is visible.
        Msg("![VK WorldMaterial] base texture NOT FOUND: '%s.dds' -> WHITE default (terrain/world will be blank)", diffuse_name);
        s_Cache.emplace(std::move(key), s_Default);
        return s_Default;
    }

    // Base diffuse — the big VRAM consumer: eligible for the texture_lod quality
    // slider AND dynamic mip streaming (r_txstream).
    // Colour: this is the base albedo for BOTH world statics and every skinned visual
    // (characters, weapons and trees all resolve through WorldMaterialCache::GetOrCreate
    // — vk_Visual.cpp), so this one call site covers essentially all lit albedo.
    // Keyed by resolved path (NOT by the material key): several materials sharing a
    // diffuse must share the image — see s_DiffuseTexCache.
    const u64 _mk0 = CPU::GetCLK();
    CVulkanTexture* tex = LoadIntoCache(s_DiffuseTexCache, std::string(full), full, nullptr,
                                        TexStreamClass::WorldDiffuse, TexColorSpace::Color, false);
    s_mkDiffuseClk += CPU::GetCLK() - _mk0;
    if (!tex) {   // present but unreadable; cached as a known failure, not retried
        s_Cache.emplace(std::move(key), s_Default);
        return s_Default;
    }

    // Terrain bases opt out of dynamic streaming (vk_terrain_cache captures the
    // splat set by handle) — which FREEZES their residency for the session. So
    // heal any load-time budget crush to FULL RES first, HERE, before any set
    // captures this texture's view (the ground is the one surface that is always
    // on screen — a frozen 256px terrain base reads as "каша", seen on Pripyat).
    if (is_terrain) {
        const u64 _c0 = CPU::GetCLK();
        VK::TextureStreamer::Instance().EnsureMinResidency(tex, 16384);   // = full chain
        VK::TextureStreamer::Instance().SetStreamable(tex, false);
        s_mkTerrainClk += CPU::GetCLK() - _c0;
    }

    // .thm lookup: R4 detail descriptor (texture + scale) and the bump
    // association. Materials with no detail association get the grey fallback
    // so the shader's `2 * base * detail` reduces to `base`.
    THMInfo         thm{};
    CVulkanTexture* detail_tex = nullptr;
    {
        const u64 _c0 = CPU::GetCLK();
        LookupTHM(diffuse_name, thm);
        detail_tex = thm.has_detail ? GetOrLoadDetailTex(thm.detail_name) : s_GreyDetail;
        s_mkThmClk += CPU::GetCLK() - _c0;
    }

    // R4 TESS_HM gate: a bump association whose `<bump>#.dds` (alpha = height)
    // actually loads. shaders.xr TessMethod is NO_TESS across stock content,
    // so the .thm bump is the practical per-material opt-in. Alpha-tested and
    // decal materials stay flat — their coverage must match the depth prepass.
    CVulkanTexture* bumpx_tex = s_FlatBump;
    if (!wmark && alphaRef < 0.0f && thm.has_bump && HeightTexNeeded()) {
        const u64 _c0 = CPU::GetCLK();
        std::string bumpx_name = thm.bump_name + "#";
        // alphaOnly: every sampler of uTexBumpX reads `.a` (POM march, the flat-material
        // early-out, the TES displacement) — the colour blocks are a measured neutral
        // 0.5 fill, so the loader repacks these BC3 files to half-size BC4.
        bumpx_tex = GetOrLoadGameTex(s_BumpTexCache, bumpx_name.c_str(), s_FlatBump,
                                     TexColorSpace::Data, TexStreamClass::Terrain, /*alphaOnly*/ true);
        // Tess diag: report every material that DID / DID NOT pick up a height
        // texture, so "why is this wall flat?" is answerable from the log.
        // (.thm has a bump assoc but the `#` height texture may be missing.)
        Msg("[VK Tess] '%s': bump '%s' -> %s ('%s#')", diffuse_name, thm.bump_name.c_str(),
            (bumpx_tex != s_FlatBump) ? "TESSELLATED (height loaded)" : "NO height tex, stays flat",
            thm.bump_name.c_str());
        s_mkBumpxClk += CPU::GetCLK() - _c0;
    }

    // Material NORMAL + GLOSS (`<bump>.dds`). Deliberately NOT behind the tess gate
    // above: that one exists because displaced geometry must match the depth prepass
    // coverage, which is irrelevant here — a normal map changes shading, never
    // coverage. So alpha-tested fences and wallmark decals get their normals too.
    // TexStreamClass::Bump = subject to the LOAD-TIME budget fit (unlike the `#` height
    // above, which stays Terrain-class and is pinned). There is one of these per
    // material, so on a big level they are a first-class VRAM consumer, not a handful
    // of tiled maps: Pripyat loads ~1590 of them, 491 MB even after the budget shrank
    // them.
    //
    // ⚠ r_bump 0 SKIPS THE LOAD ENTIRELY — it is a memory gate, not just a shading
    // gate. Gating only the shader (as this first shipped) left the whole cost in VRAM
    // for a feature that was switched off, which is why "r_bump 0" did not rescue
    // Pripyat from an out-of-memory. Read at material-load time: flipping the cvar
    // live changes shading immediately, but reclaiming/loading the textures needs a
    // level reload.
    CVulkanTexture* bumpn_tex = s_FlatBumpN;
    if (thm.has_bump && ps_r_bump > 0.0f) {
        const u64 _c0 = CPU::GetCLK();
        bumpn_tex = GetOrLoadGameTex(s_BumpNTexCache, thm.bump_name.c_str(), s_FlatBumpN,
                                     TexColorSpace::Data, TexStreamClass::Bump);
        s_mkBumpnClk += CPU::GetCLK() - _c0;
        // Tie it to this material's diffuse so the streamer can manage it: a normal
        // map gets no GPU feedback of its own, so it inherits the wanted resolution of
        // the surface it is painted on. Without the link the whole Bump class can only
        // be cut at LOAD and never recovers — on a small card that is a permanently
        // flat-shaded world, not a temporary one.
        if (bumpn_tex != s_FlatBumpN)
            VK::TextureStreamer::Instance().LinkCompanion(tex, bumpn_tex);
    }
    // Coverage diagnostic. "Two identical fences shade differently" is answerable
    // only from this: a material is missing its normal+gloss either because its .thm
    // declares no bump at all, or because the declared `<bump>.dds` failed to load —
    // and those two look the same in the gloss debug view. Counters are cumulative
    // for the process (this cache is never reset per level, by design), so the last
    // line in the log carries the running totals.
    {
        const u64 _c0 = CPU::GetCLK();
        static u32 s_bumpOk = 0, s_bumpNone = 0, s_bumpMiss = 0;
        if (!thm.has_bump)                    ++s_bumpNone;
        else if (bumpn_tex == s_FlatBumpN)    ++s_bumpMiss;
        else                                  ++s_bumpOk;
        Msg("[VK Bump] '%s': %s%s%s (loaded %u / no-assoc %u / missing %u)", diffuse_name,
            thm.has_bump ? "bump '" : "NO .thm bump association",
            thm.has_bump ? thm.bump_name.c_str() : "",
            thm.has_bump ? (bumpn_tex != s_FlatBumpN ? "' -> LOADED" : "' -> MISSING .dds") : "",
            s_bumpOk, s_bumpNone, s_bumpMiss);
        s_mkDiagClk += CPU::GetCLK() - _c0;
    }

    // Lightmap from the level shader's 3rd texture slot. Vert-lit / non-
    // lightmapped materials pass null here → white fallback (no-op multiply).
    const u64 _mkLmap0 = CPU::GetCLK();
    CVulkanTexture* lmap_tex = GetOrLoadLmapTex(lmap_name);
    s_mkLmapClk += CPU::GetCLK() - _mkLmap0;

    const u64 _mkSet0 = CPU::GetCLK();
    auto* m = xr_new<WorldMaterial>();
    m->tex          = tex;
    m->view         = tex->GetView();
    m->sampler      = s_Sampler;
    m->view_detail  = detail_tex ? detail_tex->GetView() : s_GreyDetail->GetView();
    m->detailScale  = (detail_tex && detail_tex != s_GreyDetail) ? thm.detail_scale : 0.0f;
    m->view_lmap    = lmap_tex   ? lmap_tex->GetView()   : s_WhiteLmap->GetView();
    m->alphaRef     = alphaRef;
    m->isWmark      = wmark;
    m->isWater      = wmark && alphaRef < -4.5f;                       // -5 = water body (Pass_Water)
    m->isLitBlend   = wmark && alphaRef < -3.5f && alphaRef > -4.5f;   // -4 = lit-blend (lightplanes)
    m->isEmisAdd    = wmark && alphaRef < -2.5f && alphaRef > -3.5f;   // -3 = emissive-additive marker
    m->isGlass      = wmark && alphaRef < -1.5f && alphaRef > -2.5f;   // -2 = the glass marker (vk_Visual LoadTexture)
    m->name         = diffuse_name;
    m->tessellated  = (bumpx_tex != s_FlatBump);
    m->view_bump    = bumpx_tex->GetView();
    m->view_bumpn   = bumpn_tex->GetView();
    m->hasBumpN     = (bumpn_tex != s_FlatBumpN);
    m->tex_bumpn    = (bumpn_tex != s_FlatBumpN) ? bumpn_tex : nullptr;   // streamer rebind key
    // GPU-feedback slot of the base diffuse — pushed to the world FS so it can
    // report the actually-sampled LOD (0xFFFFFFFF = not streamable, shader skips).
    m->streamID     = VK::TextureStreamer::Instance().GetFeedbackSlot(tex);
    m->set          = AllocateSet();
    if (m->set == VK_NULL_HANDLE) {
        Msg("![VK WorldMaterial] Pool exhausted creating '%s' — falling back to default", diffuse_name);
        tex->Destroy();
        xr_delete(tex);
        xr_delete(m);
        s_Cache.emplace(std::move(key), s_Default);
        return s_Default;
    }
    WriteSet(m->set, m->view, m->view_detail, m->view_lmap, bumpx_tex->GetView(), bumpn_tex->GetView());
    s_mkSetClk += CPU::GetCLK() - _mkSet0;

    // ----- Terrain splatting: diffuse under "terrain\" gets the 7-binding set.
    // Mask = "<diffuse>_mask"; details = the 4 channel defaults; detail UV
    // scale reuses the base .thm detail_scale (e.g. terrain_escape = 144).
    // Falls back gracefully: missing mask → white (even blend), so terrain is
    // never worse than the single-detail path. (is_terrain computed above, at
    // the load-time full-res heal.)
    if (is_terrain && s_TerrainSetLayout != VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = s_TerrainPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &s_TerrainSetLayout;
        VkDescriptorSet tset = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(VulkanHW.m_Device, &ai, &tset) == VK_SUCCESS) {
            string_path mask_name;
            xr_sprintf(mask_name, "%s_mask", diffuse_name);
            CVulkanTexture* mask = GetOrLoadGameTex(s_TerrainDetCache, mask_name, s_WhiteMask);

            // No real `_mask` shipped → vanilla-SoC regionalization: the LEVEL
            // SHADER names the surface type. Synthesize a one-hot mask so this
            // region gets its ONE proper detail (kDet order: R=grass, G=asphalt,
            // B=earth, A=yantar) instead of an even 4-way mush.
            if (mask == s_WhiteMask && shader_name && shader_name[0]) {
                xr_string sl = shader_name;
                std::transform(sl.begin(), sl.end(), sl.begin(), ::tolower);
                int ch = 2;   // default: earth
                if (sl.find("asfalt") != xr_string::npos || sl.find("asphalt") != xr_string::npos ||
                    sl.find("beton")  != xr_string::npos || sl.find("road")    != xr_string::npos)
                    ch = 1;
                else if (sl.find("grass") != xr_string::npos || sl.find("trav") != xr_string::npos)
                    ch = 0;
                else if (sl.find("yantar") != xr_string::npos || sl.find("sand") != xr_string::npos ||
                         sl.find("pesok")  != xr_string::npos)
                    ch = 3;
                mask = s_ChannelMask[ch];
                m->terrainChannel = (u8)ch;   // TerrainMask bake keys regions off this
                Msg("[VK Terrain] '%s' has no _mask - shader '%s' -> one-hot channel %d (%s)",
                    diffuse_name, shader_name, ch,
                    ch == 0 ? "grass" : ch == 1 ? "asphalt" : ch == 2 ? "earth" : "yantar");
            }

            const VkImageView v[15] = {
                m->view,
                mask ? mask->GetView() : s_WhiteMask->GetView(),
                s_TerrainDetail[0]->GetView(), s_TerrainDetail[1]->GetView(),
                s_TerrainDetail[2]->GetView(), s_TerrainDetail[3]->GetView(),
                m->view_lmap,
                s_TerrainNormal[0]->GetView(), s_TerrainNormal[1]->GetView(),
                s_TerrainNormal[2]->GetView(), s_TerrainNormal[3]->GetView(),
                s_TerrainHeight[0]->GetView(), s_TerrainHeight[1]->GetView(),
                s_TerrainHeight[2]->GetView(), s_TerrainHeight[3]->GetView(),
            };
            WriteTerrainSet(tset, v);
            m->isTerrain  = true;
            m->terrainSet = tset;
            // Terrain bases NEVER dynamically stream: vk_terrain_cache captures
            // `terrainSet` by handle for its composite bake — a streaming refresh
            // would retire that handle under it. (Load-time caps still apply.)
            VK::TextureStreamer::Instance().SetStreamable(tex, false);
            m->streamID = 0xFFFFFFFFu;
            // Terrain still needs a sane detail UV scale even when the base .thm
            // had none (single-detail path left it 0 → detailUV collapses).
            // Fallback 128: maps that DO ship a .thm sit around 144 (Cordon) — the
            // old 64 made detail texels ~2x larger = "stretched" ground on .thm-less
            // maps (Pripyat).
            if (m->detailScale <= 0.0f) m->detailScale = thm.detail_scale > 0.0f ? thm.detail_scale : 128.0f;
            Msg("[VK Terrain] '%s' splat set: mask=%s scale=%.0f", diffuse_name,
                (mask && mask != s_WhiteMask) ? "REAL" : "white", m->detailScale);
        }
    }

    s_Cache.emplace(std::move(key), m);
    return m;
}

}}  // namespace VK::WorldMaterialCache

// Defined out here because the implementation lives inside WorldMaterialCache's
// anonymous namespace, which only becomes reachable after that block closes.
namespace VKEditor
{
void RetryTerrainBlenderMap() { VK::WorldMaterialCache::RetryTerrainBlenderMapImpl(); }
} // namespace VKEditor
