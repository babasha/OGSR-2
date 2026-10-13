// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Pipeline cache keyed by vertex layout + shader program.
//
// Phase-2 lazy creation of forward graphics pipelines so we stop hardcoding
// stride 32 / tcOffset 24 in WorldPipeline. One VkPipelineLayout is shared
// across the whole world program (push: mat4 mvp + vec2 uvScale, no
// descriptors yet); per-Key VkPipelines are created on first Get() and
// reused thereafter.

#pragma once
#include "vk_core.h"
#include "vk_authorship.h"   // build-identity tokens folded into the key hash below

namespace VK {
namespace PipelineCache {

// World uber-FS variant bits (Inc 1). Baked as specialization constants so the
// driver dead-code-eliminates unused features (POM march, snow, wet, IBL, debug)
// per pipeline — killing their VGPR pressure, not just their runtime cost. Bit
// layout MUST match the SpecId decorations in shaders/world_variants.glsl.
enum WorldSpec : u8
{
    WS_POM   = 1u << 0,   // material has a real `#` height (per-material: mat->tessellated)
    WS_SNOW  = 1u << 1,   // snow accumulation active this frame (frame-global)
    WS_WET   = 1u << 2,   // rain/wetness active this frame (frame-global)
    WS_IBL   = 1u << 3,   // sky specular IBL active this frame (frame-global, r_ibl)
    WS_DEBUG = 1u << 4,   // any r_*_debug view active (frame-global, 0 in normal play)
    // Texture-streaming GPU feedback (txfbReport atomicMin) master gate — on
    // whenever r_txstream is (off DCEs the atomic). The atomic is an FS SIDE
    // EFFECT, which forbids automatic early-Z (measured 6-8x FS invocations vs
    // visible samples, Кордон 23-07-2026); that cost is solved structurally by
    // the EARLY_ZTEST FS twin on no-z-write statics pipelines (EarlyTwin), so
    // feedback stays per-frame.
    WS_FEEDBACK = 1u << 5,
    // DDGI probe field in the ambient term (frame-global, r_ddgi_light). Off DCEs
    // the eight-probe lookup and its 16 atlas taps out of the world FS entirely,
    // which is what makes "the volume is off" cost a surface nothing at all.
    WS_DDGI  = 1u << 6,
    // A lamp has a shadow map (spot tile / point cube) or the flashlight cookie this
    // frame (frame-global). Off DCEs the per-lamp shadow lookups out of shadeDynLight:
    // on the Switch they alone held the R1 terrain at 64 registers / half occupancy.
    WS_LAMPSH = 1u << 7,
    WS_ALL   = WS_POM | WS_SNOW | WS_WET | WS_IBL | WS_DEBUG | WS_FEEDBACK | WS_DDGI | WS_LAMPSH,   // full uber path (safe default)
    WS_FRAME = WS_SNOW | WS_WET | WS_IBL | WS_DEBUG | WS_FEEDBACK | WS_DDGI | WS_LAMPSH,            // the frame-global subset
};

// Per-frame value of the frame-global variant bits (WS_FRAME subset). Get() ORs
// this into every world key so callers only supply the per-material POM bit; the
// terrain pipeline reads it too. Defaults to WS_FRAME (full path) until Pass_World
// stamps it each frame. Weather/debug flips lazily create + cache new variants.
void SetFrameSpecMask(u8 mask);
u8   GetFrameSpecMask();

// The world variant constants (SpecId 0..8, shaders/world_variants.glsl) for `mask`,
// for a pipeline built outside this cache whose FS includes world_variants.glsl (the
// skinned one). The caller owns the storage (it must outlive the pipeline create) and
// may append its own constants after the first kWorldSpecCount entries.
constexpr u32 kWorldSpecCount = 9;
void FillWorldSpecInfo(u8 mask, bool rrGuides, VkBool32* vals, VkSpecializationMapEntry* entries,
                       VkSpecializationInfo& info);

// Pre-create the WET / SNOW / WET|SNOW variants of every world + terrain pipeline
// built so far, so the first rain/snowfall doesn't stall compiling them mid-gameplay.
// INCREMENTAL: the first call snapshots the work list; each call then creates up to
// `budget` pipelines. Cold-compile of the uber-FS is ~300ms each, so creating all at
// once froze a whole frame — spread it (budget 1/frame) instead. Returns true while
// work remains (keep calling); false when the queue is drained.
bool PrewarmWeatherVariants(u32 budget);

// Is the weather prewarm still owed work? True before the first call has snapshotted
// the list (nothing has started yet) and while the queue holds keys. The precache
// countdown asks this: those frames exist to spread these compiles, so once the
// queue is dry there is nothing left for them to warm.
bool PrewarmPending();

// Move the pipelines the prewarm worker finished into the live cache. Render
// thread only, once a frame -- Get() reads the map this writes.
void PrewarmDrain();

struct Key
{
    u32             stride       = 0;       // vertex stride in bytes (32 / 36 / 40 / 44)
    u32             tcOffset     = 24;      // TEXCOORD0 byte offset within the vertex
    VkShaderModule  vs           = VK_NULL_HANDLE;
    VkShaderModule  fs           = VK_NULL_HANDLE;
    bool            depthTest    = false;   // wires up in phase 4 (depth target)
    // Baked level decal variant: alpha blend, depth test but NO write, negative
    // depth bias (geometry is coplanar with the surface beneath — newspapers).
    bool            wmark        = false;
    // Heightmap-tessellation variant (R4 TESS_HM): VS+TCS+TES+FS over patch
    // lists; the TCS/TES pair is picked by tcOffset (lmap/vlit). Small negative
    // depth bias so the re-rasterized flat margins win ties vs the (flat)
    // depth prepass.
    bool            tess         = false;
    // Emissive-additive variant (level `effects\glow` billboards, `selflight`
    // model parts — lamp halos/projector faces): blend (SRC_ALPHA, ONE), depth
    // test but NO write, no bias. The FS goes unlit via the aref == -3 marker.
    bool            emis         = false;
    // VRS diag (r_vrs_static): bake a hard static 2x2 rate (KEEP/KEEP). In the
    // key so the cvar can flip mid-game — Get() stamps it, distinct pipelines.
    bool            vrsStatic    = false;
    // Ray Reconstruction guide G-buffer bound to the scope this pipeline draws in.
    // NOT a shader variant: the fragment shaders declare the guide outputs
    // unconditionally (writes to a location with no attachment are discarded). What
    // it keys is the ATTACHMENT LIST — dynamic rendering demands the pipeline's
    // colour-attachment count match the scope's, so a pipeline built while RR was
    // off must never be reused inside a guide-carrying scope. Get() stamps it from
    // RRGuide::Ready(), which is what makes r_dlss_rr a live toggle.
    bool            rrGuides     = false;
    // World uber-FS variant mask (Inc 1). Callers set only the per-material POM bit
    // (WS_POM); Get() stamps the frame-global bits (WS_FRAME) from SetFrameSpecMask().
    // Baked into the FS as specialization constants in CreatePipeline. Default WS_ALL
    // reproduces the monolithic shader (safe for any key that never gets stamped).
    u8              specMask     = WS_ALL;
    // Host-driven INSTANCED variant (vk_instance_gpu): adds a second, INSTANCE-rate
    // vertex binding carrying the model matrix as four vec4 attributes (locations
    // 6..9), consumed by the INSTANCED shader bodies. Everything else — layout,
    // descriptor sets, push range, blend/depth state, spec constants — is identical
    // to the normal world pipeline, which is exactly why this rides PipelineCache
    // instead of duplicating that state. Kept an explicit key field rather than
    // inferred from `vs`: the vertex input layout must stay a stated property.
    bool            instanced    = false;
    // Alpha-tested statics variant (r_at_equal): depth EQUAL + NO write. The
    // prepass already resolved the frontmost OPAQUE texel's depth, so EQUAL
    // early-Z rejects every other AT fragment — occluded layers AND the
    // transparent texels of the front layer — before the (heavy) uber-FS runs.
    // Callers set it only for aref>=0 statics that ARE in the prepass (never
    // dynamics/wmark/emis/tess — their color depth differs from the prepass).
    bool            atEqual      = false;
    // Prepass-covered statics (r_z_prepass): depth write OFF, LEQUAL kept. The
    // prepass already wrote these items' final depth, so the color-pass write is
    // redundant — and CRITICALLY, the world uber-FS statically contains `discard`
    // (runtime aref gate), which with z-write ON forces the HW to LATE-Z: every
    // occluded fragment still runs the FS. Attributed 23-07-2026 on Кордон:
    // gpuStatics = 10.8M FS invocations vs ~3.5M visible (r_fsinv_split) — the
    // whole frustum's depth complexity was being shaded. No write → early-Z is
    // legal again even with discard present → occluded fragments never launch.
    bool            noZWrite     = false;
    // r_world_bindless (10-09): the GPU world's material-agnostic groups -- the *_bl twin of vs/fs (world_bindless.glsl),
    // laid out on GetLayoutBindless() (set 0 = the bindless material set). vs/fs stay the BASE modules in the key.
    bool            bindless     = false;

    bool operator==(const Key& o) const noexcept
    {
        return stride == o.stride && tcOffset == o.tcOffset
            && vs == o.vs && fs == o.fs && depthTest == o.depthTest
            && wmark == o.wmark && tess == o.tess && emis == o.emis
            && vrsStatic == o.vrsStatic && specMask == o.specMask
            && instanced == o.instanced && atEqual == o.atEqual
            && noZWrite == o.noZWrite && rrGuides == o.rrGuides
            && bindless == o.bindless;
    }
};

// Initialised once after device creation; tears down on shutdown. Loads the
// world shader pair and creates the shared layout.
bool Init();
void Destroy();

// Lazy-creates the VkPipeline for `key` if absent, returns the cached handle.
// Returns VK_NULL_HANDLE on failure (logged).
VkPipeline Get(const Key& key);

// Shared by all draws — same push range, no descriptor sets.
VkPipelineLayout GetLayout();
// r_world_bindless: the world layout with set 0 = the bindless material set (uWorldTex[] + the material and the
// entry->material buffers, world_bindless.glsl), set 1 = EnvLight, the same push range. Null when unsupported.
VkPipelineLayout      GetLayoutBindless();
VkDescriptorSetLayout GetWorldBindlessSetLayout();
u32                   WorldBindlessMaxTextures();   // descriptor capacity of binding 0 (5 slots per material)
bool                  WorldBindlessReady();         // layout built AND every *_bl twin loaded

// The single, engine-wide VkPipelineCache object. Created (and seeded from the
// on-disk blob) in Init(), serialized back to disk + destroyed in Destroy().
// ALL pipeline producers (this cache, Pass_Sky, Pass_Skinned, ...) must pass
// this into vkCreateGraphicsPipelines so the driver dedups + warm-starts across
// runs. Returns VK_NULL_HANDLE before Init() / after Destroy() — passing that to
// vkCreateGraphicsPipelines is legal (just means "no cache"), so callers needn't
// null-check.
VkPipelineCache GetCacheObject();

// Pre-loaded module accessors so callers (Pass_World, RenderQueue::Flush)
// can build keys without touching g_ShaderManager directly. Two variants:
//   - "Lmap"  for tcOffset==24 layout: reads TC0@24 + TC1@28 (lightmap UV),
//     samples binding 2 in FS for baked sun/hemi.
//   - "Vlit"  for tcOffset==28 layout: reads COLOR@24 (D3DCOLOR pre-baked
//     RGB lighting, .a sun mask) + TC0@28; multiplies albedo by COLOR.bgr.
VkShaderModule WorldLmapVS();
VkShaderModule WorldLmapFS();
VkShaderModule WorldVlitVS();
VkShaderModule WorldVlitFS();
// Cluster-LOD crossfade variants (r_cluster_fade): fade bits via gl_InstanceIndex
// + Bayer screen-door discard. Only the GPU-driven world path binds these.
// Instanced world VS variants (Key::instanced) — VK_NULL_HANDLE when the modules
// are absent, in which case the host scene stays on the CPU RenderQueue.
VkShaderModule WorldLmapInstVS();
VkShaderModule WorldVlitInstVS();
VkShaderModule WorldLmapFadeVS();
VkShaderModule WorldLmapFadeFS();
VkShaderModule WorldVlitFadeVS();
VkShaderModule WorldVlitFadeFS();

// World heightmap tessellation (R4 TESS_HM): true when the device feature is
// enabled AND all four world TCS/TES modules loaded. Callers must not build
// keys with tess=true while this is false.
bool TessAvailable();

// Stage mask of the shared world/terrain push-constant range. The exact same
// mask MUST be passed to every vkCmdPushConstants targeting those layouts
// (VUID 01795: flags must match the range exactly). Includes the tessellation
// stages whenever the device supports them.
VkShaderStageFlags GetPushStages();

// Terrain splatting (R4 CBlender_BmmD). Single variant (stride 32, tcOffset 24,
// depth on) with its own pipeline layout (set 0 = WorldMaterialCache's 7-binding
// terrain set). Lazily built on first GetTerrainPipeline() — needs swapchain
// formats like the world pipelines. Both return VK_NULL_HANDLE before Init().
VkPipelineLayout GetTerrainLayout();
VkPipeline       GetTerrainPipeline();
// Depth-prepass variant of the terrain pipeline: same world_terrain.vert (so snow
// displacement matches color exactly -> no z-fight), vertex-only, terrain layout.
// Bind the EnvLight set at set 1 (it reads sf_params.w). VK_NULL_HANDLE before Init().
// `mv`: the CAMERA depth prepass carries an RG16F motion attachment (r_mv_prepass)
// and dynamic rendering makes the attachment list part of the pipeline. Pass true
// only from the camera prepass; the shadow casters have no colour attachment at all.
VkPipeline       GetTerrainDepthPipeline(bool mv = false);

// Sun shadow caster (STEP 2): depth-only pipelines (shadow_depth.vert, no FS),
// keyed by vertex stride (position is at offset 0 for every level layout). The
// shared layout has a single push range { mat4 lightMVP } (VERTEX). Used by
// RenderQueue::FlushDepth from Pass_SunShadow. VK_NULL_HANDLE if the shader is
// missing. The pipelines render into the shadow map's D32 format.
// `mv` (all three depth getters): the CAMERA depth prepass opens its scope with an
// RG16F motion attachment (r_mv_prepass). Under dynamic rendering the pipeline's
// colour-attachment list must match the scope's, so the prepass needs a twin that
// DECLARES the attachment — with colorWriteMask 0, because statics say nothing
// about their own motion (the resolve reconstructs it from depth) and the sentinel
// the scope cleared has to survive under them. Pass true ONLY from the camera
// prepass: these same pipelines are the sun shadow casters, whose scopes have no
// colour attachment.
VkPipelineLayout GetDepthLayout();
VkPipeline       GetDepthPipeline(u32 stride, bool mv = false);
// Solid depth + crossfade dither FS (cluster-LOD transitions) — same layout/state
// as GetDepthPipeline; vk_world_gpu::DrawDepth swaps to it when r_cluster_fade > 0.
VkPipeline       GetDepthFadePipeline(u32 stride, bool mv = false);

// Alpha-tested shadow caster variant (bushes, grates): VS passes the base UV,
// FS samples the material diffuse (set 0 = WorldMaterial set) and discards
// below alphaRef — depth keeps the foliage silhouette instead of solid quads.
// Push: { mat4 lightMVP; vec2 uvScale; float alphaRef } (VERTEX|FRAGMENT).
VkPipelineLayout GetDepthATLayout();
VkPipeline       GetDepthATPipeline(u32 stride, u32 tcOffset, bool mv = false);

// STATICS NORMAL G-buffer (GTAO): re-raster the prepass statics into the SSAO
// normal RT, depth-tested LEQUAL against the already-laid prepass depth with
// depth WRITE OFF, so GTAO reads the same vertex normal the colour pass shades
// with instead of a depth-derivative guess. Position @0 + packed normal @12 are
// at fixed offsets in every level static layout, so the solid variant keys on
// stride alone; the AT variant adds the base UV (keyed like GetDepthATPipeline)
// and the material set so grates punch out.
// Push (both): { mat4 mvp; vec4 mi; vec4 mj; vec4 misc } — 112 B. mi/mj carry
// the dynamic-prop model rows (.w = uvScale), misc.x = alphaRef, misc.y = "the
// rows are real". VK_NULL_HANDLE when world_normal*.spv are missing.
VkPipelineLayout GetNormalLayout();
VkPipeline       GetNormalPipeline(u32 stride);
VkPipelineLayout GetNormalATLayout();
VkPipeline       GetNormalATPipeline(u32 stride, u32 tcOffset);

// Push block of both statics-normal pipelines (mirrors world_normal.vert).
struct NormalPush
{
    Fmatrix mvp;
    float   mi[4];     // xyz = model row i, w = uvScale.x
    float   mj[4];     // xyz = model row j, w = uvScale.y
    float   misc[4];   // x = alphaRef, y = model rows are real, zw = pad
};
static_assert(sizeof(NormalPush) == 112, "must match world_normal.vert/.frag PC block");

}}  // namespace VK::PipelineCache

namespace std {
template <>
struct hash<VK::PipelineCache::Key>
{
    size_t operator()(const VK::PipelineCache::Key& k) const noexcept
    {
        // Seeded with the build-identity tokens (ogsr::sig) so the key domain is
        // bound to this build. The salts are deterministic compile-time immediates;
        // any fixed values hash correctly — these just happen to be load-bearing.
        size_t h = std::hash<u32>{}(k.stride ^ ogsr::sig::blumenau) ^ ogsr::sig::saratov;
        h ^= std::hash<u32>{}(k.tcOffset ^ ogsr::sig::catara) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<void*>{}((void*)k.vs)          + ogsr::sig::zefir + (h << 6) + (h >> 2);
        h ^= std::hash<void*>{}((void*)k.fs)          + ogsr::sig::maria + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.depthTest)           + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.wmark)               + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.tess)                + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.emis)                + 0x517cc1b7 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.vrsStatic)           + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<u32>{}(k.specMask)             + 0x85ebca6b + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.instanced)           + 0x27d4eb2f + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.atEqual)             + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.noZWrite)            + 0x517cc1b7 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.rrGuides)            + 0x27d4eb2f + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.bindless)            + 0x85ebca6b + (h << 6) + (h >> 2);
        return h;
    }
};
}  // namespace std
