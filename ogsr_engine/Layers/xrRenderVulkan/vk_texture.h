// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "vk_core.h"

// CRT (Render Target) is used as opaque pointer by CUserTextureRegistry.
// Real definition lives in shared Layers/xrRender/SH_RT.h.
class CRT;

namespace VK
{

// TEXTURE AUDIT (r_tex_audit / vk_texaudit) — see vk_texture.cpp for what it counts
// and why every block is read rather than sampled.
namespace TexAudit { void Dump(); }


// Streaming/residency class of a texture. Decides (a) VK_EXT_memory_priority so the
// driver evicts the right things first under VRAM pressure, (b) whether the manual
// `texture_lod` quality slider applies, and (c) whether the texture is eligible for
// dynamic mip streaming (r_txstream). Defaults to UI so existing callers (menu/font)
// stay full-resolution and never get streamed.
enum class TexStreamClass : u8
{
    UI = 0,        // menu/HUD/font atlases — crispness critical, never skip/stream
    WorldDiffuse,  // level + model base diffuse — the big VRAM consumers; slider + streaming apply
    Detail,        // R4 detail textures (tiled) — tracked, budget-fit only, not streamed
    Lmap,          // lightmaps — tracked, budget-fit only, not streamed
    Terrain,       // terrain splat detail/normal/height — tracked, budget-fit only
    Bump,          // <bump># height/error maps — tracked, budget-fit only
};

// Is a texture's content COLOR (sRGB-encoded light/albedo the eye was meant to see)
// or DATA (numbers that happen to live in a texture — normals, height, masks)?
//
// This decides whether the loader picks the _SRGB VkFormat, i.e. whether the sampler
// hardware-decodes to linear on every fetch. It is deliberately NOT TexStreamClass:
// that one is a residency/priority classifier and is already overloaded across roles
// (TexStreamClass::Terrain covers the terrain colour detail AND its _bump, _height and
// _mask alike — vk_world_material.cpp), so it cannot answer this question.
//
// No name heuristic is needed anywhere: every call site already knows the role, either
// because it built the filename itself with a role suffix ("_bump"/"_height"/"_mask")
// or because it called a role-dedicated helper (GetOrLoadLmapTex/GetOrLoadDetailTex).
// Compare UE5, which stores the same bit per asset as UTexture::SRGB, derived from
// TextureCompressionSettings at import — metadata decided once, never guessed at runtime.
//
// Data is the DEFAULT on purpose: it reproduces the historical all-UNORM pipeline, so a
// call site that is never audited keeps its old behaviour instead of silently changing.
enum class TexColorSpace : u8
{
    Data = 0,  // normals, height, gloss, splat masks, lightmaps, UI — sample raw
    Color,     // albedo/diffuse, sky, particle sprites — sRGB-encoded, decode on sample
};

/**
 * Vulkan Texture Wrapper
 *
 * Класс для работы с текстурами в Vulkan.
 * Включает VkImage, VkImageView и VkSampler.
 *
 * Поддерживаемые форматы:
 * - RGBA8 (VK_FORMAT_R8G8B8A8_UNORM)
 * - BC1/DXT1 (VK_FORMAT_BC1_RGBA_UNORM_BLOCK)
 * - BC3/DXT5 (VK_FORMAT_BC3_UNORM_BLOCK)
 * - BC5 (VK_FORMAT_BC5_UNORM_BLOCK)
 *
 * Usage:
 *   CVulkanTexture tex;
 *   tex.CreateFromData(pixels, 256, 256, VK_FORMAT_R8G8B8A8_UNORM);
 *   // ... use tex.GetView() and tex.GetSampler() for binding
 *   tex.Destroy();
 */
class CVulkanTexture
{
public:
    CVulkanTexture();
    ~CVulkanTexture();

    /**
     * Создать пустую текстуру
     * @param width Ширина
     * @param height Высота
     * @param format VkFormat
     * @param mipLevels Количество mip уровней (1 = без mips)
     * @param usage Image usage flags
     */
    void Create(u32 width, u32 height, VkFormat format, u32 mipLevels = 1,
                VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

    /**
     * Создать текстуру из RGBA данных
     * @param data Указатель на пиксели (RGBA или compressed)
     * @param width Ширина
     * @param height Высота
     * @param format VkFormat
     * @param dataSize Размер данных в байтах (для compressed текстур)
     */
    void CreateFromData(const void* data, u32 width, u32 height, VkFormat format,
                        VkDeviceSize dataSize = 0);

    /**
     * Загрузить текстуру из DDS файла
     * @param filename Путь к файлу
     * @param applyBCSwizzle  Apply R↔B swap on BC formats (true for UI atlases
     *                        whose tooling shipped BGR-ordered endpoints; false
     *                        for level statics which are stock BC1/3 RGB).
     * @return true если успешно
     */
    bool LoadDDS(const char* filename, bool applyBCSwizzle = true,
                 TexStreamClass streamClass = TexStreamClass::UI,
                 TexColorSpace colorSpace = TexColorSpace::Data);

    /**
     * Declare that only this texture's ALPHA is ever sampled (height maps).
     * Call BEFORE LoadDDS: it lets the loader drop the colour blocks and repack
     * a BC3 source into half-size BC4, re-routing A<-R in the view. See m_AlphaOnly.
     */
    void SetAlphaOnly(bool v) { m_AlphaOnly = v; }

    /**
     * Загрузить cubemap текстуру из DDS файла (6 faces)
     * @param filename Путь к файлу
     * @param applyBCSwizzle  R↔B swap on BC formats — see LoadDDS notes.
     * @param colorSpace      Color vs data — see TexColorSpace.
     * @return true если успешно
     */
    bool LoadDDSCubemap(const char* filename, bool applyBCSwizzle = true,
                        TexColorSpace colorSpace = TexColorSpace::Data);

    /**
     * Уничтожить текстуру
     */
    void Destroy();

    /**
     * Transition image layout
     * Используется для подготовки текстуры к transfer или sampling
     */
    void TransitionLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout);

    /**
     * Transition layout используя временный command buffer
     */
    void TransitionLayoutImmediate(VkImageLayout oldLayout, VkImageLayout newLayout);

    // Getters
    VkImage GetImage() const { return m_Image; }
    VkImageView GetView() const { return m_ImageView; }
    VkSampler GetSampler() const { return m_Sampler; }
    u32 GetWidth() const { return m_Width; }
    u32 GetHeight() const { return m_Height; }
    u32 GetMipLevels() const { return m_MipLevels; }
    VkFormat GetFormat() const { return m_Format; }
    VkImageLayout GetCurrentLayout() const { return m_CurrentLayout; }
    bool IsValid() const { return m_Image != VK_NULL_HANDLE; }
    bool IsCubemap() const { return m_bCubemap; }

    // Cubemap configuration (must be set before Create())
    bool            m_bCubemap = false;      // True if this is a cubemap texture (6 faces)
    u32             m_ArrayLayers = 1;       // Number of array layers (6 for cubemap)

    /**
     * Upload данных через staging buffer. Low-level — most callers go via
     * CreateFromData / LoadDDS / LoadDDSCubemap. Public so cubemap fallbacks
     * (sky pass) can fill 1×1×6 faces without rolling their own DDS.
     */
    void UploadData(const void* data, VkDeviceSize size);

    // --- Texture streaming support (see vk_texture_stream.{h,cpp}) --------------

    // Memory-priority (VK_EXT_memory_priority, 0..1) for this texture's VMA
    // allocation. Set BEFORE Create(); streamable world diffuse gets ~0.25 so the
    // driver spills it first. Ignored when the extension is absent.
    float m_MemPriority = 0.5f;

    // Atomically exchange every GPU handle + descriptor-visible field with `other`,
    // WITHOUT touching either object's registration. The streamer uses this to swap
    // a freshly-built mip-range image into a live CVulkanTexture (keeping the object
    // identity — and thus WorldMaterial::tex pointers — stable) while the old handles
    // migrate into `other` for deferred destruction.
    void SwapContents(CVulkanTexture& other);

    // Re-open this texture's source .dds and build a NEW image covering mips
    // [mipSkip .. end] into `out` (out must be freshly constructed/empty). Does NOT
    // touch `this`. Returns false (and leaves `out` empty) on any failure so the
    // caller can keep the current residency. Used by the streamer to promote/demote.
    bool BuildStreamImage(CVulkanTexture& out, u32 mipSkip) const;

    // Same as BuildStreamImage but parses an already-read .dds file blob (whole
    // file, magic included) instead of touching the filesystem. The streamer's IO
    // worker reads files off-thread; the render thread only pays image creation +
    // staging the copy.
    bool BuildStreamImageFromBlob(CVulkanTexture& out, u32 mipSkip,
                                  const void* blob, size_t blobSize) const;

    // Two-phase variant for the streamer's IO thread (Switch: creating + binding one image costs ~0.6-2.5 ms in NVK
    // ioctls, and up to 6 swaps a frame ran on the main thread). Phase 1, any thread: parse, repack and CREATE the
    // image from a parameter snapshot -- never touches a live texture. Phase 2, the thread that owns the upload
    // batch: FinishDeferredUpload() stages the copy, before the swap, exactly as the one-phase path does. `blob`
    // must stay alive until phase 2. (std::string, not shared_str: its refcount is not atomic.)
    struct StreamBuildParams
    {
        std::string   file;
        TexStreamClass cls       = TexStreamClass::UI;
        bool          swizzle    = true;
        bool          alphaOnly  = false;
        TexColorSpace colorSpace = TexColorSpace::Data;
        float         priority   = 0.5f;
    };
    StreamBuildParams GetStreamBuildParams() const;
    static bool PrebuildStreamImageFromBlob(CVulkanTexture& out, const StreamBuildParams& src, u32 mipSkip,
                                            const void* blob, size_t blobSize);
    void FinishDeferredUpload();

    // Source .dds path this texture was loaded from ("" for procedural/RT/video).
    const char* GetSourceFile() const { return m_SourceFile.c_str(); }

private:
    /**
     * Создать VkImageView
     */
    void CreateImageView();

    /**
     * Создать VkSampler
     */
    void CreateSampler();

    /**
     * Рассчитать количество mip levels
     */
    static u32 CalculateMipLevels(u32 width, u32 height);

    /**
     * Build the full mip chain for a cubemap from mip-0 face data, synchronously
     * (staging copy + vkCmdBlitImage per level, all 6 layers). The image must
     * already be created with the full mip count and TRANSFER_SRC usage.
     * Used so the sky cubemaps (shipped single-mip) can be sampled at a blurred
     * high mip as diffuse sky irradiance (R4 hmodel.h CUBE_MIPS).
     */
    void GenerateMipsCube(const void* mip0Data, VkDeviceSize mip0Size);

    // Parse `filename`, create the image covering mips [mipSkip..end], upload it.
    // Pure worker shared by LoadDDS (mipSkip resolved from the streamer plan) and
    // BuildStreamImage (explicit mipSkip). Does NOT register with the streamer.
    // Outputs the on-disk full-chain metadata so the caller can register/track.
    struct DDSLoadResult
    {
        bool         ok           = false;
        u32          fullW        = 0;
        u32          fullH        = 0;
        u32          fullMips     = 0;
        VkFormat     format       = VK_FORMAT_UNDEFINED;
        u32          residentBase = 0;      // mip actually uploaded (== applied skip)
        VkDeviceSize residentBytes= 0;
    };
    // mipSkip == UINT32_MAX → resolve via TextureStreamer::PlanLoadMipSkip.
    DDSLoadResult loadDDSToImage(const char* filename, bool applyBCSwizzle, u32 mipSkip,
                                 TexColorSpace colorSpace = TexColorSpace::Data);
    // Same parse/create/upload from an in-memory .dds file image (whole file,
    // magic included). `filename` is only for logging + VMA alloc naming.
    DDSLoadResult loadDDSFromMemory(const char* filename, const void* blob, size_t blobSize,
                                    bool applyBCSwizzle, u32 mipSkip,
                                    TexColorSpace colorSpace = TexColorSpace::Data);

    /**
     * Проверить является ли формат compressed (BC/DXT)
     */
    static bool IsCompressedFormat(VkFormat format);

    /**
     * Получить размер блока для compressed формата
     */
    static u32 GetBlockSize(VkFormat format);

private:
    // PrebuildStreamImageFromBlob: the upload kept for FinishDeferredUpload (data points into the caller's blob or
    // into m_DeferredOwned, a repacked copy this texture frees).
    bool            m_DeferUpload   = false;
    const void*     m_DeferredData  = nullptr;
    VkDeviceSize    m_DeferredSize  = 0;
    void*           m_DeferredOwned = nullptr;

    VkImage         m_Image       = VK_NULL_HANDLE;
    VmaAllocation   m_Allocation  = VK_NULL_HANDLE;
    VkImageView     m_ImageView   = VK_NULL_HANDLE;
    VkSampler       m_Sampler     = VK_NULL_HANDLE;

    u32             m_Width       = 0;
    u32             m_Height      = 0;
    u32             m_MipLevels   = 1;
    VkFormat        m_Format      = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout   m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool            m_bAlphaSwizzle = false; // For alpha-only textures (fonts): swizzle R→A, RGB→ONE
    bool            m_bBCSwizzle = false;    // For BC/DXT textures: swizzle R<->B for DirectX compatibility
    // Channel fix-ups for textures we sample through ONE channel while the file
    // stores four. Two cases, both repacked to single-channel BC4 on load (see
    // TranscodeBC3AlphaToBC4) and re-routed here so no shader has to change:
    //   HemiFromAlpha — CS/CoP hemi lightmap, repack skipped: broadcast A into RGB
    //   HemiFromRed   — same lightmap after repack: broadcast R into RGB
    //   AlphaFromRed  — height map (`<bump>#`, sampled as .a) after repack: A <- R
    enum class ChanFix : u8 { None = 0, HemiFromAlpha, HemiFromRed, AlphaFromRed };
    ChanFix         m_ChanFix = ChanFix::None;

    // "Only this texture's ALPHA is ever sampled" — set by the caller before
    // LoadDDS (height maps). Licenses the BC4 repack: the colour blocks are dead
    // weight by construction, not by measurement. Kept as state so a streaming
    // rebuild of the same source repacks identically.
    bool            m_AlphaOnly = false;

    // Streaming metadata — populated by LoadDDS, consumed by the streamer.
    shared_str      m_SourceFile;            // resolved .dds path (for re-reading mips)
    TexStreamClass  m_StreamClass = TexStreamClass::UI;
    bool            m_Registered  = false;   // true while present in the streamer registry
    bool            m_LoadSwizzleIntent = true;  // applyBCSwizzle used at load (for reload)
    TexColorSpace   m_LoadColorSpace = TexColorSpace::Data;  // colourspace used at load (for reload)
};

/**
 * User Texture Registry
 *
 * Регистрация динамических render targets как $user$ текстур.
 * Используется для 3D Fluid системы и других эффектов, которые создают
 * временные RT и хотят их сэмплировать в других шейдерах.
 *
 * Usage:
 *   g_UserTextureRegistry.Register("$user$Texture_velocity0", &m_RT_Velocity);
 *   CRT* rt = g_UserTextureRegistry.Get("$user$Texture_velocity0");
 */
class CUserTextureRegistry
{
public:
    /**
     * Зарегистрировать RT как $user$ текстуру
     * @param name Имя текстуры (должно начинаться с "$user$")
     * @param rt Указатель на CRT
     */
    void Register(const char* name, CRT* rt);

    /**
     * Получить RT по имени
     * @param name Имя текстуры
     * @return Указатель на CRT или nullptr если не найдено
     */
    CRT* Get(const char* name);

    /**
     * Удалить регистрацию
     * @param name Имя текстуры
     */
    void Unregister(const char* name);

    /**
     * Очистить все регистрации
     */
    void Clear();

private:
    xr_map<shared_str, CRT*> m_UserTextures;
};

// Глобальный instance
extern CUserTextureRegistry g_UserTextureRegistry;
// ============================================================================
// Level texture prefetch.
//
// A level load opens ~2460 .dds files and, measured on pripyat_full, spends
// 2860 of the texture phase's 6565 ms inside FS.r_open alone — one thread, one
// file at a time, while fifteen cores idle. Nothing about that work is ordered:
// the files are independent, and WHICH files are needed is known the moment the
// level shader table exists, long before the visual walk that consumes them.
//
// So workers open them ahead of the walk and park the ready IReader here; the
// loader's own `FS.r_open` becomes a lookup. Deliberately a BYTE cache and not
// a texture cache: the same files get loaded by the same call sites with the
// same parameters in the same order — only the bytes arrive earlier. A miss is
// simply the old path, so a wrong guess about what to prefetch costs I/O, never
// a wrong pixel.
//
// Bounded by r_tex_prefetch_mb: workers stall when the parked bytes exceed it
// and resume as the walk takes them.
// ============================================================================
// What a prefetched file IS. The old contract carried a bare path, so the only
// thing a worker could do with a file was read it and park it: turning those bytes
// into a texture (which stream class, which colour space, which cache owns it)
// needed knowledge that lived on the loading thread. The role travels with the
// path, so a worker can finish the job instead of handing it over half-done.
enum class TexRole : u8 { Diffuse = 0, Detail, Bump, Height, Mask, Lmap };

struct TexJob
{
    std::string path;            // resolved full path on disk
    std::string key;             // key the owning cache in vk_world_material uses
    TexRole     role = TexRole::Diffuse;
};

// Fills `out` with every texture file one material's diffuse pulls in (the
// diffuse itself, its .thm detail, its bump normal + height, its terrain mask).
// Supplied by vk_world_material.cpp — that is where the resolution rules live.
using ExpandFn = void (*)(const char* diffuseName, xr_vector<TexJob>& out);

// Load ONE job to completion on the calling worker: read, create the image, upload
// it and publish it into its cache, so the walk finds it instead of building it.
// Also supplied by vk_world_material.cpp (the caches are its). r_tex_materialize 0
// falls back to the old behaviour: read the file and park it for the walk.
using LoadFn = void (*)(const TexJob& job);

namespace TexPrefetch
{
// True on a texture-prefetch worker. Read by the BC3->BC4 gather, which must not
// recruit helpers from the shared farm when sixteen workers are already running.
extern thread_local bool t_TexWorker;

// `diffuseNames` = level shader table bases (expanded by `expand` on a worker);
// `directPaths` = already-resolved files (lightmaps). No-op if r_tex_prefetch 0.
//
// `leadFile` is prefaulted BEFORE any of that, by all workers at once, front to
// back. It is not parked — the point is only to get its pages resident, because
// its reader (level.geom, through a 1 MB sliding window) is sequential and single
// threaded and spends all its time faulting: measured 1869 MB at 705 MB/s with the
// window remaps costing 8 ms of it. Workers outrun that reader immediately, so the
// lead file needs no head start; it just has to go first, since the geometry load
// runs before the visual walk that wants the textures.
void Start(xr_vector<shared_str>&& diffuseNames, xr_vector<TexJob>&& directJobs, ExpandFn expand,
           LoadFn materialize, const char* leadFile = nullptr);


// Close a reader on a worker instead of here. Measured on pripyat_full: closing the
// 2463 texture files costs 394 ms of the 956 ms texture phase — an archived file
// frees its whole decompressed image, a loose one unmaps 4.7 GB of view between them,
// and both happen on the main thread between two texture loads. Bounded by bytes in
// flight: past the cap the caller closes inline, so this can never grow the peak.
void CloseAsync(IReader*& F);

// Wait for the deferred closes to finish. Called where the level would otherwise
// unload underneath them.
void CloseDrain();

// The lead file's whole-file view -- the mapping the workers prefault, front to
// back. A caller that has to read the same file should read it out of THIS view:
// a mapping of its own costs a page fault per 4 KB on its own thread (measured on
// level.geom: 465 ms of a 644 ms stage), while these pages are being faulted in by
// sixteen workers ahead of the read. Null unless `path` IS the lead file.
const u8* LeadView(const char* path, u64& size);

// Ready reader for `fullPath`, ownership transferred (caller FS.r_close's it),
// or null when it was never parked. Safe to call with the prefetch stopped.
IReader* Take(const char* fullPath);

// Let the workers start BUILDING textures (r_tex_materialize 2). Until this is
// called they only read and park, which is all they should do while the geometry
// stage owns the upload ring and the memory bus: measured, texture uploads mixed
// into that stage drag the whole ring from 18.5 to 8.4 GB/s and the geometry pays
// every millisecond the walk saves. Called where the visual walk begins.
void EnableMaterialize();

// Join workers, close whatever nobody took, log the hit rate.
void Stop();
}   // namespace TexPrefetch



} // namespace VK
