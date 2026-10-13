// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "../../xr_3da/IGame_Level.h" // g_pGameLevel: [tex late] only in gameplay
#include "vk_texture.h"
#include "vk_texture_stream.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"
#include "vk_parallel.h"       // VK::FarmRun — the persistent pool the BC4 repack splits over
#include "vk_clk.h"            // VK::ClkToMs — rdtsc counters inside the repack
#include "../../xrCore/load_prof.h"   // per-spawn breakdown (tex.read/parse/create/upload)
#include "../../xrCore/render_thread.h" // [tex late]: which thread read it
#include "HW_Vulkan.h"
#ifdef __SWITCH__
extern "C" void xr_nx_backtrace(const char* tag); // nx/xr_nx_throw.cpp
#endif
#ifdef _MSC_VER
#include <emmintrin.h>         // SSE2 gather (BC3 alpha half -> BC4); other compilers take the memcpy branch
#endif

#include <utility>   // std::swap (SwapContents)
#include <thread>              // TexPrefetch worker pool
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <deque>                // deferred-close queue

#include <string>
#include <cctype>              // tolower — case-insensitive path keys

// r_tex_prefetch / r_tex_prefetch_mb — see vk_console_min.cpp. Global scope on
// purpose: a namespace-scope extern mangles differently and silently fails to bind.
extern int ps_r_tex_prefetch;
extern int ps_r__tf_Anisotropic;      // r__tf_aniso — texture sampler anisotropy (vk_console_min.cpp, default 16)
extern int ps_r_tex_prefetch_mb;
extern int ps_r_tex_prefetch_lmaps_first;   // park the level's lightmaps before the diffuse bases
extern int ps_r_tex_repack_threads;   // helpers for the BC3->BC4 gather (0 = caller alone)
extern int ps_r_tex_repack_scratch;   // reuse one destination buffer per thread instead of malloc per texture
extern int ps_r_tex_audit;            // r_tex_audit — scan every BC3 block at load and count what could be repacked
extern int ps_r_tex_repack_verify;    // rebuild each gather the plain way and compare
extern int ps_r_tex_materialize;      // 1 = prefetch workers build the texture, not just read the file
extern int ps_r_tex_mat_threads;      // how many of them may be building at once (0 = all)


// r_linear_color — 1 = load TexColorSpace::Color textures as _SRGB so the sampler
// decodes to linear. Declared at GLOBAL scope on purpose: a namespace-scope extern
// mangles differently and silently fails to bind (see the SSAO lesson).
#include "vk_color_space.h"   // ColorSpace::Active() — latched: format choice must not flip mid-session

// DDS definitions
const u32 DDS_MAGIC = 0x20534444; // "DDS "

struct DDS_PIXELFORMAT {
    u32 dwSize;
    u32 dwFlags;
    u32 dwFourCC;
    u32 dwRGBBitCount;
    u32 dwRBitMask;
    u32 dwGBitMask;
    u32 dwBBitMask;
    u32 dwABitMask;
};

struct DDS_HEADER {
    u32 dwSize;
    u32 dwFlags;
    u32 dwHeight;
    u32 dwWidth;
    u32 dwPitchOrLinearSize;
    u32 dwDepth;
    u32 dwMipMapCount;
    u32 dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    u32 dwCaps;
    u32 dwCaps2;
    u32 dwCaps3;
    u32 dwCaps4;
    u32 dwReserved2;
};

// FourCC codes
const u32 FOURCC_DXT1 = 0x31545844; // "DXT1"
const u32 FOURCC_DXT3 = 0x33545844; // "DXT3"
const u32 FOURCC_DXT5 = 0x35545844; // "DXT5"
const u32 FOURCC_DX10 = 0x30315844; // "DX10" — extended header (DDS_HEADER_DXT10) follows

// Present immediately after DDS_HEADER when ddspf.dwFourCC == "DX10". Modern
// compressors (BC7/BC6H, and BC1-5 re-saved by texconv) emit this instead of the
// legacy FourCC, carrying an explicit DXGI format.
struct DDS_HEADER_DXT10 {
    u32 dxgiFormat;
    u32 resourceDimension;
    u32 miscFlag;
    u32 arraySize;
    u32 miscFlags2;
};

// Flags
const u32 DDPF_ALPHAPIXELS = 0x1;
const u32 DDPF_ALPHA       = 0x2;
const u32 DDPF_FOURCC      = 0x4;
const u32 DDPF_RGB         = 0x40;
const u32 DDPF_LUMINANCE   = 0x20000;

namespace VK
{

// Load-time counters live in TexLoadProf further down this file; Create/UploadData
// run above it, so the three they feed are declared here. rdtsc, not CTimer: a
// sampler create is a few microseconds and CTimer would truncate most of them to
// zero (see vk_clk.h).
namespace TexLoadProf {
extern std::atomic<u64> s_createVramClk, s_createViewClk, s_createSampClk, s_uploadRegionsClk;
extern std::atomic<u32> s_createDedicated;
}

// Constructor
CVulkanTexture::CVulkanTexture()
{
}

// Destructor
CVulkanTexture::~CVulkanTexture()
{
    Destroy();
}

// Создание пустой текстуры
void CVulkanTexture::Create(u32 width, u32 height, VkFormat format, u32 mipLevels,
                            VkImageUsageFlags usage)
{
    VK::Vram::Scope _vram_scope("Textures");
    if (m_Image != VK_NULL_HANDLE) {
        Msg("![Vulkan] Texture already created, call Destroy first");
        return;
    }

    if (width == 0 || height == 0) {
        Msg("![Vulkan] Cannot create texture with size 0");
        return;
    }

    m_Width = width;
    m_Height = height;
    m_Format = format;
    m_MipLevels = mipLevels > 0 ? mipLevels : 1;

    // Image create info
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = m_MipLevels;
    imageInfo.arrayLayers = m_ArrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = m_bCubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;

    // Upload-target images are filled on the dedicated TRANSFER queue and sampled
    // on GRAPHICS. With distinct families that's cross-family access — CONCURRENT
    // sharing lets the contents (and the transfer-side layout transition) survive
    // without queue-ownership-transfer barriers, which the transfer queue can't
    // express for the final SHADER_READ transition anyway (no FRAGMENT_SHADER
    // stage). Sampled textures aren't DCC-compressed, so the cost is negligible.
    // Only for TRANSFER_DST images when the families actually differ.
    const u32 imgFamilies[2] = { VulkanHW.m_GraphicsFamily, VulkanHW.m_TransferFamily };
    if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
        VulkanHW.m_TransferFamily != VulkanHW.m_GraphicsFamily) {
        imageInfo.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        imageInfo.queueFamilyIndexCount = 2;
        imageInfo.pQueueFamilyIndices   = imgFamilies;
    }

    // VMA allocation info - prefer device local memory
    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // VK_EXT_memory_priority: streamable world diffuse carries a low priority (set by
    // LoadDDS) so the driver spills IT to system RAM first under VRAM pressure, before
    // touching render targets / geometry. Ignored when the extension is absent.
    allocInfo.priority = m_MemPriority;

    // Big images (ANY class) get a DEDICATED VkDeviceMemory. Sub-allocated into
    // shared 256 MB blocks, a freed texture leaves a HOLE the driver still counts
    // as used (measured 15-07: ~596 MB demoted → 43 MB of usage back) — that
    // accumulated into the ~2 GB "untracked" VMA slack that starved NGX (the
    // mid-game DLSS enable 0xbad0000d) and pushed the card into overcommit.
    // Dedicated => destroy/demote is an actual release to the OS, for streaming
    // swaps AND level transitions (bumps/lmaps are the biggest pooled residents).
    // Small images stay pooled — device allocation-count hygiene (~4096 limit;
    // blocks were ~889 on Pripyat, big textures add well under 1.5k).
    {
        VkDeviceSize approx = 0;
        u32 w = width, h = height;
        for (u32 i = 0; i < mipLevels; ++i) {
            if (IsCompressedFormat(format))
                approx += (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * GetBlockSize(format);
            else
                approx += (VkDeviceSize)w * h * 4;
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
        if (approx >= (2ull << 20)) {
            allocInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            ++VK::TexLoadProf::s_createDedicated;
        }
    }

    {
        const u64 _c0 = CPU::GetCLK();
        VK_CHECK(VK::Vram::CreateImage(VulkanHW.m_Allocator, &imageInfo, &allocInfo,
                                &m_Image, &m_Allocation, nullptr));
        VK::TexLoadProf::s_createVramClk += CPU::GetCLK() - _c0;
    }

    // VK_CHECK is non-fatal (logs only). If the allocation failed, m_Image is
    // VK_NULL_HANDLE — bail before CreateImageView/CreateSampler, which would
    // otherwise build a view over a null image (invalid usage) and leak a sampler.
    if (m_Image == VK_NULL_HANDLE) {
        Msg("![Vulkan] Texture image allocation failed: %ux%u fmt=%d", width, height, (int)format);
        return;
    }

    m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Создаём view и sampler
    { const u64 _c0 = CPU::GetCLK(); CreateImageView(); VK::TexLoadProf::s_createViewClk += CPU::GetCLK() - _c0; }
    { const u64 _c0 = CPU::GetCLK(); CreateSampler();   VK::TexLoadProf::s_createSampClk += CPU::GetCLK() - _c0; }

    // Msg("[Vulkan] Texture created: %dx%d, format=%d, mips=%d", width, height, format, m_MipLevels);
}

// Создание текстуры из данных
void CVulkanTexture::CreateFromData(const void* data, u32 width, u32 height, VkFormat format,
                                    VkDeviceSize dataSize)
{
    if (!data) {
        Msg("![Vulkan] CreateFromData: data is null");
        return;
    }

    // Рассчитываем размер данных если не указан
    if (dataSize == 0) {
        if (IsCompressedFormat(format)) {
            // Для compressed форматов нужен явный размер
            Msg("![Vulkan] CreateFromData: dataSize required for compressed format");
            return;
        }
        // Для RGBA8 - 4 bytes per pixel
        dataSize = width * height * 4;
    }

    // Создаём текстуру
    Create(width, height, format, 1);
    if (m_Image == VK_NULL_HANDLE) {
        return;
    }
    if (m_Allocation) {  // leak-dump name: procedural textures have no file name
        char nm[32]; xr_sprintf(nm, "proc:%ux%u", width, height);
        vmaSetAllocationName(VulkanHW.m_Allocator, m_Allocation, nm);
    }

    // Upload данных
    UploadData(data, dataSize);
}

// Upload данных через staging buffer
void CVulkanTexture::UploadData(const void* data, VkDeviceSize size)
{
    // Prepare copy regions for mipmaps (and array layers for cubemaps).
    // DDS cubemap layout: for each face, all mipmaps sequentially. bufferOffsets are
    // relative to `data`; the async uploader rebases them into its staging ring.
    xr_vector<VkBufferImageCopy> regions;
    VkDeviceSize offset = 0;
    const u64 _regions0 = CPU::GetCLK();

    for (u32 layer = 0; layer < m_ArrayLayers; layer++) {
        u32 currentWidth = m_Width;
        u32 currentHeight = m_Height;

        for (u32 i = 0; i < m_MipLevels; i++) {
            VkBufferImageCopy region = {};
            region.bufferOffset = offset;
            region.bufferRowLength = 0;   // Tightly packed
            region.bufferImageHeight = 0; // Tightly packed
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = i;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {currentWidth, currentHeight, 1};

            regions.push_back(region);

            // Calculate size of current mip
            VkDeviceSize currentSize = 0;
            if (IsCompressedFormat(m_Format)) {
                u32 blockSize = GetBlockSize(m_Format);
                u32 blocksX = (currentWidth + 3) / 4;
                u32 blocksY = (currentHeight + 3) / 4;
                currentSize = blocksX * blocksY * blockSize;
            } else {
                // Bytes per pixel depends on format
                u32 bpp = 4; // Default: RGBA8/BGRA8
                if (m_Format == VK_FORMAT_R8_UNORM)
                    bpp = 1;
                else if (m_Format == VK_FORMAT_R8G8_UNORM)
                    bpp = 2;
                currentSize = currentWidth * currentHeight * bpp;
            }

            offset += currentSize;

            // Next mip dimensions
            if (currentWidth > 1) currentWidth /= 2;
            if (currentHeight > 1) currentHeight /= 2;
        }
    }

    VK::TexLoadProf::s_uploadRegionsClk += CPU::GetCLK() - _regions0;

    // Async upload on the dedicated transfer queue — no per-texture command pool, no
    // vkQueueWaitIdle, no per-upload staging buffer. Leaves the image in
    // SHADER_READ_ONLY_OPTIMAL; the graphics frame's upload-timeline wait
    // (FRAGMENT_SHADER) makes the copy+transition visible to samplers. The image is
    // created CONCURRENT{graphics,transfer} so no queue-ownership transfer is needed.
    CommandManager.UploadImage(m_Image, data, size, regions.data(), (u32)regions.size(),
                               m_MipLevels, m_ArrayLayers);
    m_CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Transition image layout
void CVulkanTexture::TransitionLayout(VkCommandBuffer cmd, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_Image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = m_MipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = m_ArrayLayers;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else {
        Msg("![Vulkan] Unsupported layout transition: %d -> %d", oldLayout, newLayout);
        return;
    }

    vkCmdPipelineBarrier(cmd, sourceStage, destinationStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    m_CurrentLayout = newLayout;
}

// Transition layout с выделенным immediate command buffer
// Uses dedicated cmd buffer to avoid corrupting the render frame's command buffer.
void CVulkanTexture::TransitionLayoutImmediate(VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkCommandBuffer cmd = CommandManager.BeginImmediate();
    if (cmd == VK_NULL_HANDLE) {
        Msg("![Vulkan] TransitionLayoutImmediate: failed to begin immediate cmd");
        return;
    }
    TransitionLayout(cmd, oldLayout, newLayout);
    CommandManager.EndAndSubmitImmediate(cmd);
}

// Создание ImageView
void CVulkanTexture::CreateImageView()
{
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_Image;
    viewInfo.viewType = m_bCubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_Format;
    if (m_bAlphaSwizzle) {
        // Alpha-only texture (fonts): R channel → alpha, RGB = white
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_ONE;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_ONE;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_ONE;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_R;
    } else if (m_ChanFix == ChanFix::AlphaFromRed) {
        // Height map repacked to BC4: shaders sample `.a`, the scalar now lives in R.
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_R;
    } else if (m_ChanFix != ChanFix::None) {
        // CS/CoP hemi lightmap: broadcast the single meaningful channel into RGB
        // so the SHoC-convention shaders (`dot(lm.rgb, 1/3)`) read the real hemi.
        // Red once repacked to BC4, alpha if the repack was skipped.
        const VkComponentSwizzle hs = (m_ChanFix == ChanFix::HemiFromRed) ? VK_COMPONENT_SWIZZLE_R
                                                                          : VK_COMPONENT_SWIZZLE_A;
        viewInfo.components.r = hs;
        viewInfo.components.g = hs;
        viewInfo.components.b = hs;
        viewInfo.components.a = hs;
    } else if (m_bBCSwizzle) {
        // BC/DXT textures: swap R<->B channels
        // DirectX DXT textures use BGRA order, Vulkan BC uses RGBA
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_B;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
    } else {
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = m_MipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = m_ArrayLayers;

    VK_CHECK(vkCreateImageView(VulkanHW.m_Device, &viewInfo, nullptr, &m_ImageView));
}

// Создание Sampler
// Using CLAMP_TO_EDGE to match R3/R4 DX behavior (shader:dx10sampler("smp_base"):clamp())
void CVulkanTexture::CreateSampler()
{
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    if (m_bCubemap) {
        // Cubemaps must use CLAMP_TO_EDGE to avoid seam artifacts
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    } else {
        // Use REPEAT for game textures (walls, terrain tile beyond UV [0,1])
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
    // r__tf_aniso (default 16 = the old fixed value). The Switch preset sets 1: 16x aniso costs a lot of texel bandwidth
    // on GM20B. Read when the texture's sampler is created (texture load), not live.
    {
        const int aniso = ps_r__tf_Anisotropic < 1 ? 1 : (ps_r__tf_Anisotropic > 16 ? 16 : ps_r__tf_Anisotropic);
        samplerInfo.anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy    = float(aniso);
    }
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(m_MipLevels);

    VK_CHECK(vkCreateSampler(VulkanHW.m_Device, &samplerInfo, nullptr, &m_Sampler));
}

// Уничтожение текстуры
void CVulkanTexture::Destroy()
{
    // A prebuilt stream image dropped before its upload: free the repacked copy it kept.
    if (m_DeferredOwned) { xr_free(m_DeferredOwned); m_DeferredOwned = nullptr; }
    m_DeferredData = nullptr;
    m_DeferredSize = 0;

    // Drop out of the streamer registry first (only LoadDDS-built textures are in it;
    // procedural / streaming-temp images never registered so this is a no-op there).
    if (m_Registered) {
        VK::TextureStreamer::Instance().Unregister(this);
        m_Registered = false;
    }

    if (m_Sampler != VK_NULL_HANDLE) {
        vkDestroySampler(VulkanHW.m_Device, m_Sampler, nullptr);
        m_Sampler = VK_NULL_HANDLE;
    }

    if (m_ImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(VulkanHW.m_Device, m_ImageView, nullptr);
        m_ImageView = VK_NULL_HANDLE;
    }

    if (m_Image != VK_NULL_HANDLE) {
        VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_Image, m_Allocation);
        m_Image = VK_NULL_HANDLE;
        m_Allocation = VK_NULL_HANDLE;
    }

    m_Width = 0;
    m_Height = 0;
    m_MipLevels = 1;
    m_CurrentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    m_bCubemap = false;
    m_ArrayLayers = 1;
}

// Map a DXGI_FORMAT (from a DX10 extended DDS header) to the VkFormat we load it
// as. The engine runs an UNORM pipeline (UNORM swapchain, no gamma hardware), so
// sRGB DXGI variants are loaded as their UNORM equivalents — same convention the
// Legacy (pre-DX10) FourCC -> VkFormat. Shared by the 2D loader and the cubemap
// loader, which are otherwise separate implementations (the cubemap path never
// calls loadDDSFromMemory) and each carried its own copy of this switch — so a
// newly supported block format could land in one and not the other. Returns
// VK_FORMAT_UNDEFINED for anything unhandled; the caller words its own message and
// does its own cleanup, which is all the two sites ever actually differed in.
//
// DXT1 always maps to the RGBA (punch-through alpha) form: many X-Ray DDS files
// omit DDPF_ALPHAPIXELS yet still use the 1-bit alpha, and D3D11 treats DXT1 as
// alpha-capable regardless, so matching it keeps the two renderers in agreement.
static VkFormat FourCCToVkFormat(u32 fourCC)
{
    switch (fourCC) {
        case FOURCC_DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case FOURCC_DXT3: return VK_FORMAT_BC2_UNORM_BLOCK;
        case FOURCC_DXT5: return VK_FORMAT_BC3_UNORM_BLOCK;
        default:          return VK_FORMAT_UNDEFINED;
    }
}

// legacy BC1/2/3 path already uses. Returns VK_FORMAT_UNDEFINED for formats we
// don't handle so the caller can bail with a clear message.
static VkFormat DXGIFormatToVk(u32 dxgi)
{
    switch (dxgi) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8_UNORM:            return VK_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8G8_UNORM:          return VK_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:      return VK_FORMAT_BC2_UNORM_BLOCK;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:      return VK_FORMAT_BC3_UNORM_BLOCK;
        case DXGI_FORMAT_BC4_UNORM:           return VK_FORMAT_BC4_UNORM_BLOCK;
        case DXGI_FORMAT_BC4_SNORM:           return VK_FORMAT_BC4_SNORM_BLOCK;
        case DXGI_FORMAT_BC5_UNORM:           return VK_FORMAT_BC5_UNORM_BLOCK;
        case DXGI_FORMAT_BC5_SNORM:           return VK_FORMAT_BC5_SNORM_BLOCK;
        case DXGI_FORMAT_BC6H_UF16:           return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case DXGI_FORMAT_BC6H_SF16:           return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:      return VK_FORMAT_BC7_UNORM_BLOCK;
        default:                              return VK_FORMAT_UNDEFINED;
    }
}

// The _SRGB twin of a colour-capable UNORM format, or the format unchanged when it has
// no sRGB flavour (BC4/BC5/BC6H, R8/R8G8 — single/dual-channel data that is never colour).
//
// Applied as ONE post-step after the format-selection chain in loadDDSFromMemory so all
// three header paths (DX10 / legacy FourCC / uncompressed RGB) are covered by one rule
// instead of three parallel edits. Note the sRGB twin is always block/byte-size identical
// to its UNORM base, so every mip-size, copy-region and VRAM-accounting computation
// downstream stays valid — IsCompressedFormat/GetBlockSize already enumerate the _SRGB
// block formats, as do the streamer's mirrors in vk_texture_stream.cpp.
static VkFormat ToSrgbFormat(VkFormat f)
{
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM:       return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:       return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:  return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case VK_FORMAT_BC2_UNORM_BLOCK:      return VK_FORMAT_BC2_SRGB_BLOCK;
        case VK_FORMAT_BC3_UNORM_BLOCK:      return VK_FORMAT_BC3_SRGB_BLOCK;
        case VK_FORMAT_BC7_UNORM_BLOCK:      return VK_FORMAT_BC7_SRGB_BLOCK;
        default:                             return f;
    }
}

// Priority (VK_EXT_memory_priority) by streaming class: the classes the budget can
// shrink are cheapest to evict; everything else keeps the VMA mid default. Bump joined
// them on 25-07 — one map per material makes them bulk, not fixtures.
static float PriorityForClass(VK::TexStreamClass k)
{
    return (k == VK::TexStreamClass::WorldDiffuse || k == VK::TexStreamClass::Bump) ? 0.25f : 0.5f;
}

// Which channel of a hemi lightmap actually holds the bake?
//
// X-Ray has TWO lightmap conventions and the shaders pick one at COMPILE time
// (common_functions.h): SHoC does `get_hemi = dot(lmh.rgb, 1/3)` with sun in .a,
// CS/CoP does `get_hemi = lmh.a` with sun in .g. A level built by the later
// compiler therefore ships lmap#N_2 with RGB left EMPTY — and read the SHoC way
// it yields hemi == 0, i.e. every lightmapped static on the map loses its sky
// light and renders as if it stood in the dark (measured: Pripyat's hemi maps are
// rgb 0.000 / alpha 0.250, Cordon's are rgb 0.177 / alpha 0.128).
//
// Rather than branch per level, detect it per texture from the data itself and let
// the image VIEW normalise the convention (RGB←A), so the shaders stay single-form.
// BC3 endpoints are enough: means over a sample of blocks, no decode needed.
static bool DetectLmapHemiChannel(const void* data, VkDeviceSize dataSize,
                                  u32 width, u32 height, VkFormat format,
                                  double& outRgbMean, double& outAlphaMean)
{
    if (format != VK_FORMAT_BC3_UNORM_BLOCK && format != VK_FORMAT_BC3_SRGB_BLOCK) return false;

    const u64 blocks = (u64)((width + 3) / 4) * ((height + 3) / 4);
    if (blocks == 0 || dataSize < blocks * 16) return false;

    const u8* p = (const u8*)data;
    const u64 step = (blocks > 4096) ? (blocks / 4096) : 1;   // ~4k samples is plenty
    double sumRGB = 0.0, sumA = 0.0;
    u64 n = 0;
    for (u64 b = 0; b < blocks; b += step, ++n) {
        const u8* blk = p + b * 16;
        sumA += (blk[0] + blk[1]) * (0.5 / 255.0);                  // BC3 alpha endpoints
        const u16 c0 = (u16)(blk[8]  | (blk[9]  << 8));             // BC1 colour endpoints (RGB565)
        const u16 c1 = (u16)(blk[10] | (blk[11] << 8));
        const double r = (((c0 >> 11) & 31) + ((c1 >> 11) & 31)) * (0.5 / 31.0);
        const double g = (((c0 >>  5) & 63) + ((c1 >>  5) & 63)) * (0.5 / 63.0);
        const double bl = ((c0 & 31) + (c1 & 31)) * (0.5 / 31.0);
        sumRGB += (r + g + bl) * (1.0 / 3.0);
    }
    if (n == 0) return false;

    outRgbMean = sumRGB / n;
    outAlphaMean = sumA / n;
    // Deliberately lopsided thresholds: the two conventions are an order of
    // magnitude apart, so only a genuinely EMPTY rgb (with data in alpha) flips.
    return (outRgbMean < 0.02) && (outAlphaMean > 0.02);
}

// Repack such a lightmap BC3 -> BC4, dropping the empty colour blocks.
//
// This is NOT a re-encode: BC4 *is* BC3's alpha block — the same 8 bytes, same
// two endpoints, same 3-bit indices. So the repack is a pure gather (keep 8 of
// every 16 bytes) with no decoder, no encoder, no generation loss; the sampler
// reads bit-identical values afterwards. What we drop is measured to be exactly
// zero. Pripyat's 27 x 4096^2 hemi maps: 432 MB -> 216 MB.
// Defined with the rest of the load table further down; used here, above it.
namespace TexLoadProf { extern std::atomic<u64> s_repackAllocClk; extern std::atomic<u32> s_repackMismatch; }

// Two blocks at a time: BC3 keeps the alpha half in bytes 0..7 of every 16, so
// the gather is exactly _mm_unpacklo_epi64 of two consecutive blocks — 32 bytes
// in, 16 out, no shuffle table and no branch. SSE2, so no CPU feature test.
// ⚠Портируемая ветка -- НЕ SIMD, а два memcpy по 8 байт: это ровно то же самое
// значение, что даёт unpacklo_epi64 (младшие 64 бита каждого блока подряд),
// SSE здесь только ради скорости широкой загрузки/записи, не семантики.
static void GatherAlphaBlocks(u8* d, const u8* s, u64 blocks)
{
    u64 b = 0;
#ifdef _MSC_VER
    for (; b + 2 <= blocks; b += 2, s += 32, d += 16) {
        const __m128i b0 = _mm_loadu_si128((const __m128i*)(s +  0));
        const __m128i b1 = _mm_loadu_si128((const __m128i*)(s + 16));
        _mm_storeu_si128((__m128i*)d, _mm_unpacklo_epi64(b0, b1));
    }
#else
    for (; b + 2 <= blocks; b += 2, s += 32, d += 16) {
        memcpy(d + 0, s +  0, 8);
        memcpy(d + 8, s + 16, 8);
    }
#endif
    if (b < blocks) memcpy(d, s, 8);
}

static void* TranscodeBC3AlphaToBC4(const void* src, u32 width, u32 height, u32 mipLevels,
                                    VkDeviceSize& outSize, bool& outOwned)
{
    outOwned = true;
    // Mip table first: with it a destination byte range maps back to a source
    // range without walking the chain, which is what lets the gather be split.
    struct MipSpan { u64 srcOff, dstOff, blocks; };
    MipSpan  mips[16];
    u32      nMips = 0;
    u64      srcOff = 0, dstOff = 0, totalBlocks = 0;
    for (u32 w = width, h = height, i = 0; i < mipLevels && nMips < 16; ++i, ++nMips) {
        const u64 blocks = (u64)((w + 3) / 4) * ((h + 3) / 4);
        mips[nMips] = { srcOff, dstOff, blocks };
        srcOff += blocks * 16;
        dstOff += blocks * 8;
        totalBlocks += blocks;
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }
    const VkDeviceSize total = (VkDeviceSize)dstOff;

    // Split the allocation off the gather. Four helper threads moved this whole
    // function by nothing at all, which means the ceiling is not cores -- and half
    // a gigabyte of FRESH pages per load (a fault per 4 KB, on write) is the first
    // suspect that a thread split cannot touch.
    const u64 _cAlloc0 = CPU::GetCLK();
    u8* dst = nullptr;
    if (ps_r_tex_repack_scratch) {
        // One buffer per thread, grown to the largest image and kept. 561 repacks a
        // load allocate half a gigabyte between them and free it again immediately;
        // reusing the pages means faulting them once instead of once per texture.
        // The caller must not free it -- hence outOwned.
        static thread_local u8*    s_scratch     = nullptr;
        static thread_local size_t s_scratchSize = 0;
        if (s_scratchSize < (size_t)total) {
            if (s_scratch) xr_free(s_scratch);
            s_scratch     = (u8*)xr_malloc((size_t)total);
            s_scratchSize = s_scratch ? (size_t)total : 0;
        }
        dst      = s_scratch;
        outOwned = false;
    } else {
        dst = (u8*)xr_malloc((size_t)total);
    }
    VK::TexLoadProf::s_repackAllocClk += CPU::GetCLK() - _cAlloc0;
    if (!dst) return nullptr;

    const u8* s = (const u8*)src;

    // One thread reads 16 bytes and writes 8 for every block, and the 27 hemi maps
    // of a level are 4096^2 each: 561 repacks measured 183 ms on the loading thread
    // while fifteen cores sat idle. The pool is the persistent one (no threads
    // created per texture), and the grain is chosen so a chunk is worth the claim.
    constexpr u64 kGrainBlocks = 8192;   // 128 KB in, 64 KB out
    u32 helpers = ps_r_tex_repack_threads < 0 ? 0 : (u32)ps_r_tex_repack_threads;
    if (helpers > 15) helpers = 15;
    // ...unless we ARE one of sixteen workers already (r_tex_materialize): the farm
    // is shared and one caller at a time holds it, so recruiting from inside it turns
    // a parallel phase back into a queue.
    if (VK::TexPrefetch::t_TexWorker) helpers = 0;
    const u32 chunks = (u32)((totalBlocks + kGrainBlocks - 1) / kGrainBlocks);

    if (helpers == 0 || chunks <= 1) {
        for (u32 i = 0; i < nMips; ++i)
            GatherAlphaBlocks(dst + mips[i].dstOff, s + mips[i].srcOff, mips[i].blocks);
        outSize = total;
        return dst;
    }

    VK::FarmRun(chunks, helpers, [&](u32 c) {
        const u64 first = (u64)c * kGrainBlocks;
        const u64 last  = _min(first + kGrainBlocks, totalBlocks);
        // Which mip owns this range — at most 16 entries, so a scan per chunk.
        u64 base = 0;
        for (u32 i = 0; i < nMips; ++i) {
            const u64 lo = _max(first, base), hi = _min(last, base + mips[i].blocks);
            if (lo < hi)
                GatherAlphaBlocks(dst + mips[i].dstOff + (lo - base) * 8,
                                  s   + mips[i].srcOff + (lo - base) * 16, hi - lo);
            base += mips[i].blocks;
            if (base >= last) break;
        }
    });

    // Guard (r_tex_repack_verify): the SIMD gather and the chunk split both changed
    // WHICH bytes land where, and a wrong lightmap is not a crash — it is a level
    // that looks slightly off. Rebuild the same buffer with the plain per-block copy
    // this replaced and compare. Off by default; one run with it on is the proof.
    if (ps_r_tex_repack_verify) {
        if (u8* ref = (u8*)xr_malloc((size_t)total)) {
            const u8* rs = s;
            u8*       rd = ref;
            for (u32 i = 0; i < nMips; ++i)
                for (u64 b = 0; b < mips[i].blocks; ++b, rs += 16, rd += 8)
                    memcpy(rd, rs, 8);
            if (memcmp(ref, dst, (size_t)total) != 0) {
                ++VK::TexLoadProf::s_repackMismatch;
                Msg("![VK Repack] gather MISMATCH: %ux%u mips=%u (%llu blocks) — the SIMD/threaded path is wrong",
                    width, height, mipLevels, (unsigned long long)totalBlocks);
            }
            xr_free(ref);
        }
    }

    outSize = total;
    return dst;
}


// ============================================================================
// TEXTURE AUDIT (r_tex_audit / vk_texaudit) — WHERE ELSE IS A CHANNEL BEING PAID
// FOR AND NOT USED.
//
// This engine already repacks two narrow classes from BC3 to BC4 (see
// TranscodeBC3AlphaToBC4): hemi lightmaps whose bake sits in alpha, and the height
// maps flagged m_AlphaOnly. Both are gated by CLASS, not by content — so nothing
// has ever looked at the rest of the level's BC3 and asked whether it is paying 8
// bits per texel for channels it does not use.
//
// Two opportunities, and the second has never been counted:
//   * RGB EMPTY -> the alpha half alone IS a BC4 block, bit for bit. Halves it.
//   * ALPHA FULLY OPAQUE -> the COLOUR half alone IS a BC1 block, bit for bit
//     (BC3 is literally a BC4 alpha block followed by a BC1 colour block). Also
//     halves it, and X-Ray content is full of DXT5 saved by artists who never
//     used the alpha channel.
//
// ⚠⚠ EVERY BLOCK IS READ, NOT A SAMPLE. The tree card survey learned this the
// expensive way: a 7-point sample claimed 11.1% of leaf cards were droppable when
// the truth was 0.2% — a factor of 55 — and building on it would have punched
// holes in the crowns. "This texture never uses alpha" is exactly the kind of
// claim a sparse sample says yes to and a single block says no to.
//
// ⚠ The BC1 rewrite additionally needs every colour block to have c0 > c1: a BC3
// colour block is always 4-colour, but reinterpreted as BC1 a block with c0 <= c1
// silently becomes 3-colour-plus-transparent. Counted apart, so the report can
// never suggest a repack that would punch holes.
// ============================================================================
namespace TexAudit {

enum Fmt { F_BC1, F_BC2, F_BC3, F_BC4, F_BC5, F_BC6, F_BC7, F_RGBA8, F_OTHER, F_COUNT };
static const char* kName[F_COUNT] = { "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "RGBA8", "other" };

// Sixteen prefetch workers load textures, so every counter is an atomic — the same
// rule TexLoadProf follows a few hundred lines up.
static std::atomic<u32> s_count[F_COUNT];
static std::atomic<u64> s_bytes[F_COUNT];
static std::atomic<u64> s_bc3RgbEmpty{0}, s_bc3Opaque{0}, s_bc3OpaqueUnsafe{0}, s_bc3Real{0};
static std::atomic<u32> s_bc3RgbEmptyN{0}, s_bc3OpaqueN{0}, s_bc3RealN{0};
// What the SHIPPING gate already repacks, kept apart from everything else. Without
// this split the report bills a saving the engine has been taking for months as a
// new opportunity — the first run of this instrument did exactly that.
static std::atomic<u64> s_alreadyBytes{0};
static std::atomic<u32> s_alreadyN{0};
static std::atomic<u32> s_scanned{0};

static Fmt ClassifyFmt(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:  case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return F_BC1;
    case VK_FORMAT_BC2_UNORM_BLOCK:      case VK_FORMAT_BC2_SRGB_BLOCK:      return F_BC2;
    case VK_FORMAT_BC3_UNORM_BLOCK:      case VK_FORMAT_BC3_SRGB_BLOCK:      return F_BC3;
    case VK_FORMAT_BC4_UNORM_BLOCK:      case VK_FORMAT_BC4_SNORM_BLOCK:     return F_BC4;
    case VK_FORMAT_BC5_UNORM_BLOCK:      case VK_FORMAT_BC5_SNORM_BLOCK:     return F_BC5;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:    case VK_FORMAT_BC6H_SFLOAT_BLOCK:   return F_BC6;
    case VK_FORMAT_BC7_UNORM_BLOCK:      case VK_FORMAT_BC7_SRGB_BLOCK:      return F_BC7;
    case VK_FORMAT_R8G8B8A8_UNORM:       case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:       case VK_FORMAT_B8G8R8A8_SRGB:       return F_RGBA8;
    default:                                                                 return F_OTHER;
    }
}

// Walks the base mip's blocks. bit 1 = alpha is 255 everywhere, bit 2 = rgb
// endpoints are empty, bit 4 = every colour block is BC1-safe (c0 > c1).
static u32 ScanBC3(const void* data, VkDeviceSize dataSize, u32 w, u32 h)
{
    const u64 blocks = (u64)((w + 3) / 4) * ((h + 3) / 4);
    if (blocks == 0 || dataSize < blocks * 16) return 0;
    const u8* p = (const u8*)data;

    bool   opaque = true, bc1safe = true;
    double sumRGB = 0.0, sumA = 0.0;

    for (u64 b = 0; b < blocks; ++b) {
        const u8* blk = p + b * 16;
        const u8  a0 = blk[0], a1 = blk[1];
        sumA += (a0 + a1) * (0.5 / 255.0);

        if (opaque) {
            // ⚠ a0 == a1 == 255 is NOT enough. With a0 <= a1 the block uses the
            // six-interpolant mode, where index 6 decodes to 0 and index 7 to 255 —
            // so a block with both endpoints opaque can still contain fully
            // transparent texels. The indices have to be read.
            if (a0 != 255 || a1 != 255) opaque = false;
            else {
                u64 bits = 0;
                for (int i = 0; i < 6; ++i) bits |= (u64)blk[2 + i] << (8 * i);
                for (int t = 0; t < 16; ++t)
                    if (((bits >> (3 * t)) & 7u) == 6u) { opaque = false; break; }
            }
        }

        const u16 c0 = (u16)(blk[8]  | (blk[9]  << 8));
        const u16 c1 = (u16)(blk[10] | (blk[11] << 8));
        if (c0 <= c1) bc1safe = false;
        const double r  = (((c0 >> 11) & 31) + ((c1 >> 11) & 31)) * (0.5 / 31.0);
        const double g  = (((c0 >>  5) & 63) + ((c1 >>  5) & 63)) * (0.5 / 63.0);
        const double bl = ((c0 & 31) + (c1 & 31)) * (0.5 / 31.0);
        sumRGB += (r + g + bl) * (1.0 / 3.0);
    }

    const double rgbMean = sumRGB / (double)blocks;
    const double aMean   = sumA   / (double)blocks;
    u32 flags = 0;
    if (opaque)                         flags |= 1;
    if (rgbMean < 0.02 && aMean > 0.02) flags |= 2;
    if (bc1safe)                        flags |= 4;
    return flags;
}

// One call per loaded texture. `bytes` is the whole mip chain as uploaded.
void Note(VkFormat format, u32 w, u32 h, VkDeviceSize bytes, const void* data, VkDeviceSize dataSize,
          bool alreadyRepacked)
{
    const Fmt f = ClassifyFmt(format);
    s_count[f].fetch_add(1, std::memory_order_relaxed);
    s_bytes[f].fetch_add((u64)bytes, std::memory_order_relaxed);
    if (f != F_BC3 || data == nullptr) return;
    if (alreadyRepacked) {
        s_alreadyBytes += (u64)bytes; ++s_alreadyN;
        return;                       // counted, but never as a NEW opportunity
    }

    s_scanned.fetch_add(1, std::memory_order_relaxed);
    const u32 c = ScanBC3(data, dataSize, w, h);
    if (c & 2) {
        s_bc3RgbEmpty += (u64)bytes; ++s_bc3RgbEmptyN;
    } else if (c & 1) {
        if (c & 4) { s_bc3Opaque += (u64)bytes; ++s_bc3OpaqueN; }
        else         s_bc3OpaqueUnsafe += (u64)bytes;
    } else {
        s_bc3Real += (u64)bytes; ++s_bc3RealN;
    }
}

void Dump()
{
    double total = 0.0;
    u32    nTex  = 0;
    for (int i = 0; i < F_COUNT; ++i) { total += (double)s_bytes[i].load(); nTex += s_count[i].load(); }
    if (total <= 0.0) {
        Msg("![VK TexAudit] nothing recorded — r_tex_audit must be 1 BEFORE the textures load "
            "(set it, then load the level); an empty report is not a finding");
        return;
    }
    Msg("[VK TexAudit] %u textures, %.1f MB of texture memory as loaded", nTex, total / 1048576.0);
    for (int i = 0; i < F_COUNT; ++i) {
        const u64 b = s_bytes[i].load();
        if (b == 0) continue;
        Msg("[VK TexAudit]   %-6s %5u tex | %8.1f MB (%4.1f%%)", kName[i], s_count[i].load(),
            b / 1048576.0, 100.0 * (double)b / total);
    }
    const double rgbE = (double)s_bc3RgbEmpty.load();
    const double op   = (double)s_bc3Opaque.load();
    const double opU  = (double)s_bc3OpaqueUnsafe.load();
    const double real = (double)s_bc3Real.load();
    Msg("[VK TexAudit] BC3 breakdown (%u scanned, EVERY block read — not a sample):", s_scanned.load());
    Msg("[VK TexAudit]   already repacked today  %4u tex | %7.1f MB (hemi lmaps + height maps) — NOT a new prize",
        s_alreadyN.load(), (double)s_alreadyBytes.load() / 1048576.0);
    Msg("[VK TexAudit]   rgb EMPTY      %4u tex | %7.1f MB -> BC4 saves %.1f MB",
        s_bc3RgbEmptyN.load(), rgbE / 1048576.0, rgbE / 2097152.0);
    Msg("[VK TexAudit]   alpha OPAQUE   %4u tex | %7.1f MB -> BC1 saves %.1f MB",
        s_bc3OpaqueN.load(), op / 1048576.0, op / 2097152.0);
    Msg("[VK TexAudit]   alpha opaque but some block has c0<=c1 (NOT BC1-safe): %.1f MB", opU / 1048576.0);
    Msg("[VK TexAudit]   genuinely RGBA %4u tex | %7.1f MB", s_bc3RealN.load(), real / 1048576.0);
    Msg("[VK TexAudit] => the prize is %.1f MB of VRAM%s", (rgbE + op) / 2097152.0,
        ((rgbE + op) / 2097152.0 < 8.0) ? "  <<< under 8 MB: not worth a repack path >>>" : "");
}

} // namespace TexAudit
// Pure DDS worker: parse header, create the image covering mips [mipSkip..end], and
// upload it. Shared by the public LoadDDS (mipSkip resolved from the streamer plan)
// and BuildStreamImage (explicit mipSkip for a promote/demote). Does NOT register
// with the streamer — the caller owns that. Returns ok=false on any failure with the
// image left as it was (unbuilt) so callers can fall back to a default.
// ---------------------------------------------------------------------------
// Stage 1 profiling: texture loading owns 8.5 s of a 21 s pripyat_full load
// (2028 material-cache misses x ~4.2 ms). Before choosing a fix, split ONE
// texture load into its four costs - a mip cap only helps the ones downstream
// of the read, because r_open materializes the whole file either way.
// ---------------------------------------------------------------------------
// (already inside `namespace VK` — opened at the top of this file)
namespace TexLoadProf {

// Sixteen prefetch workers write these as well as the loading thread
// (r_tex_materialize), so every counter is an atomic -- and rdtsc rather than
// CTimer, which truncates a sub-microsecond sample to zero (see vk_clk.h).
std::atomic<u64> s_totalClk{0}, s_openClk{0}, s_repackClk{0}, s_createClk{0}, s_uploadClk{0};
// The four costs above left a third of the total unaccounted for (893 vs 522 ms on
// pripyat_full). The two that were never timed: parsing the header out of the blob,
// and CLOSING the file -- which for an archived texture frees the whole decompressed
// image and for a loose one unmaps it.
std::atomic<u64> s_parseClk{0}, s_closeClk{0};
std::atomic<u32> s_count{0}, s_repacked{0};
std::atomic<u64> s_bytesRead{0}, s_bytesUploaded{0};
// How much the repack actually moves. Without it "repack 183 ms" cannot be told
// apart from "the gather is bandwidth-bound and already at the ceiling".
std::atomic<u64> s_repackBytes{0};
// And of that time, how much is the destination allocation rather than the copy,
// plus how many of the repacked bytes the residency skip throws away right after.
std::atomic<u64> s_repackAllocClk{0}, s_repackSkipBytes{0};
// `create` and `upload` were single numbers; both are sums of parts that behave
// differently under a fix (an image allocation is a driver call, a sampler is a
// per-texture object nothing outside the UI ever binds, the region walk is pure CPU).
std::atomic<u64> s_createVramClk{0}, s_createViewClk{0}, s_createSampClk{0}, s_uploadRegionsClk{0};
std::atomic<u32> s_createDedicated{0};
// Written from vk_vram_stats.cpp: the small-image probe that runs BEFORE every
// non-dedicated allocation, and how many images it actually routed to a pool.
std::atomic<u64> s_createProbeClk{0};
std::atomic<u32> s_createSmall{0};
std::atomic<u32> s_repackMismatch{0};   // r_tex_repack_verify: textures where the SIMD gather disagreed

void Dump()
{
    const float k = VK::ClkToMs();
    Msg("[load step]   texture load: %u files, %llu MB read -> %llu MB uploaded | total %.0f ms = open/read %.0f + parse %.0f + repack %.0f (%u tex, %llu MB in) + create %.0f + upload %.0f + close %.0f",
        s_count.load(), (unsigned long long)(s_bytesRead.load() >> 20), (unsigned long long)(s_bytesUploaded.load() >> 20),
        k * float(s_totalClk.load()), k * float(s_openClk.load()), k * float(s_parseClk.load()),
        k * float(s_repackClk.load()), s_repacked.load(), (unsigned long long)(s_repackBytes.load() >> 20),
        k * float(s_createClk.load()), k * float(s_uploadClk.load()), k * float(s_closeClk.load()));
    Msg("[load step]     create split: image %.0f (%u dedicated, %u small-pool; probe %.0f ms of the image) | view %.0f | sampler %.0f ms | of the upload: regions %.0f ms",
        k * float(s_createVramClk.load()), s_createDedicated.load(), s_createSmall.load(),
        k * float(s_createProbeClk.load()),
        k * float(s_createViewClk.load()), k * float(s_createSampClk.load()),
        k * float(s_uploadRegionsClk.load()));
    Msg("[load step]     repack split: xr_malloc %.0f ms of the %.0f | %llu MB gathered, %llu MB of it thrown away by the mip skip%s",
        k * float(s_repackAllocClk.load()), k * float(s_repackClk.load()),
        (unsigned long long)((s_repackBytes.load() / 2) >> 20), (unsigned long long)(s_repackSkipBytes.load() >> 20),
        ps_r_tex_repack_verify ? (s_repackMismatch.load() ? " | VERIFY: MISMATCHES" : " | verify: byte-identical") : "");
    s_totalClk = s_openClk = s_repackClk = s_createClk = s_uploadClk = s_parseClk = s_closeClk = 0;
    s_count = s_repacked = 0;
    s_bytesRead = s_bytesUploaded = s_repackBytes = s_repackSkipBytes = 0;
    s_repackAllocClk = 0; s_repackMismatch = 0;
    s_createVramClk = s_createViewClk = s_createSampClk = s_uploadRegionsClk = 0;
    s_createDedicated = 0; s_createProbeClk = 0; s_createSmall = 0;
}

// Adds its lifetime to `acc` — used for whole-function totals with many returns.
struct scope
{
    u64               t0;
    std::atomic<u64>& acc;
    explicit scope(std::atomic<u64>& a) : t0(CPU::GetCLK()), acc(a) {}
    ~scope() { acc.fetch_add(CPU::GetCLK() - t0, std::memory_order_relaxed); }
};

}   // namespace TexLoadProf

// ---------------------------------------------------------------------------
// Level texture prefetch — see the contract in vk_texture.h.
// ---------------------------------------------------------------------------
namespace TexPrefetch {

// Set once per worker thread in WorkerBody; read by the repack (and anything else
// that must not assume it owns every core).
thread_local bool t_TexWorker = false;

namespace {

std::mutex                                  s_mx;
std::condition_variable                     s_cvSpace;      // workers wait for the walk to take bytes
std::unordered_map<std::string, IReader*>   s_ready;        // key = lower-cased full path
std::unordered_set<std::string>             s_produced;     // every key ever parked (see ParkOne)

u64                                         s_parked = 0;   // bytes currently held
u64                                         s_budget = 0;
bool                                        s_running = false;

xr_vector<shared_str>  s_names;
xr_vector<TexJob>      s_direct;
ExpandFn               s_expand = nullptr;
LoadFn                 s_load   = nullptr;
std::atomic<u32>       s_materialized{0};   // jobs a worker finished instead of parking
std::atomic<bool>      s_matOn{false};      // r_tex_materialize 2: set when the walk starts
std::mutex             s_matMx;
std::condition_variable s_matCv;
u32                    s_matBusy = 0;       // workers currently inside a build (r_tex_mat_threads)
std::atomic<u32>       s_nextName{0}, s_nextDirect{0};
xr_vector<std::thread> s_pool;

// Stats (read once, in Stop)
std::atomic<u32> s_opened{0}, s_hits{0}, s_misses{0}, s_dropped{0};
std::atomic<u32> s_touchSink{0};   // consumes the page-touch loop so the optimiser keeps it

std::atomic<u64> s_openedB{0}, s_hitB{0};
float            s_wallMs = 0.f;
float            s_joinMs = 0.f;   // time the walk spent waiting for the workers in Stop()
u32              s_threads = 0;

// Windows paths are case-insensitive and the two sides of this cache are built
// by different code (FS.update_path here, the material cache there) — key on a
// lower-cased copy so a case difference can never turn into a silent miss.
std::string KeyOf(const char* p)
{
    std::string k(p);
    for (char& c : k) c = (char)tolower((unsigned char)c);
    return k;
}

// ---- Deferred close ----------------------------------------------------------
// The destructor of a reader is not free: CTempReader frees the decompressed archive
// blob, CVirtualFileReader unmaps the file and closes two handles. 2463 of those in a
// row was the third-largest item in the texture phase and it is pure teardown — nothing
// downstream waits on it. Hand it to a worker, with a byte cap so a slow drain cannot
// hold more memory than the inline path would have.
std::mutex              s_clMx;
std::condition_variable s_clCv, s_clIdle;
std::deque<IReader*>    s_clQ;
u64                     s_clBytes = 0;      // bytes waiting to be freed
u32                     s_clBusy  = 0;      // workers inside a close
bool                    s_clStop  = false;
// ⚠⚠The two close workers live for the whole run and nothing ever stopped them, so
// at exit this vector was destroyed holding joinable threads -- std::terminate, the
// same FATAL ERROR "_terminate" as s_teardown below. Stopped and joined here. The
// mutex and condition variable above are declared first, so they are destroyed
// after this and are still alive while it waits.
struct ClosePool
{
    xr_vector<std::thread> v;
    bool empty() const { return v.empty(); }
    template <class F> void emplace_back(F&& f) { v.emplace_back(std::forward<F>(f)); }
    ~ClosePool()
    {
        {
            std::lock_guard<std::mutex> lk(s_clMx);
            s_clStop = true;
        }
        s_clCv.notify_all();
        for (auto& t : v)
            if (t.joinable())
                t.join();
    }
} s_clPool;
constexpr u64           kCloseCapBytes = 192ull << 20;

void CloseWorker()
{
    for (;;) {
        IReader* F = nullptr;
        {
            std::unique_lock<std::mutex> lk(s_clMx);
            s_clCv.wait(lk, [] { return s_clStop || !s_clQ.empty(); });
            if (s_clQ.empty()) { if (s_clStop) return; continue; }
            F = s_clQ.front(); s_clQ.pop_front(); ++s_clBusy;
        }
        const u64 len = (u64)F->length();
        FS.r_close(F);
        {
            std::lock_guard<std::mutex> lk(s_clMx);
            s_clBytes -= _min(s_clBytes, len);
            --s_clBusy;
        }
        s_clIdle.notify_all();
    }
}

// Opens one file and parks it. Returns false when the prefetch is shutting down.
bool ParkOne(const std::string& path)
{
    const std::string key = KeyOf(path.c_str());
    {   // Produced once, ever. Materials share bump/detail maps heavily, and the
        // walk's own caches mean a shared file is asked for exactly once — so a
        // second park would never be claimed. Tracked apart from `s_ready`, which
        // the walk empties as it goes.
        std::lock_guard<std::mutex> lk(s_mx);
        if (!s_produced.insert(key).second) return true;
    }


    IReader* F = FS.r_open(path.c_str());
    if (!F) return true;                       // missing file: the walk resolves it the same way

    const u64 len = (u64)F->length();

    // MATERIALIZE it. A loose .dds comes back as a mapped VIEW: r_open is three
    // syscalls and the actual disk read happens on first touch — which, before this,
    // was the loader thread's staging copy, where it showed up as a mysteriously slow
    // "memcpy" (2 GB/s on memory that benchmarks far faster). Archive entries are
    // already materialized and this just warms them. One byte per 4 KB page is enough
    // to fault the whole file in, and doing it HERE is the entire point of the
    // prefetch: the read belongs on a worker, not on the thread the level waits for.
    if (const void* base = F->pointer()) {
        const u8* p = (const u8*)base;
        u32 sink = 0;
        for (size_t o = 0, n = (size_t)len; o < n; o += 4096) sink += p[o];
        s_touchSink.fetch_add(sink, std::memory_order_relaxed);   // keep the loop
    }

    std::unique_lock<std::mutex> lk(s_mx);
    // Back-pressure: hold the line until the walk has taken enough away. The
    // budget is what keeps a 3.5 GB texture set from becoming 3.5 GB of RAM.
    // ...or until the walk starts and this worker's job changes from parking bytes
    // to building textures (r_tex_materialize 2): the budget must not hold it there.
    s_cvSpace.wait(lk, [] { return !s_running || s_parked < s_budget
                                   || s_matOn.load(std::memory_order_relaxed); });
    if (!s_running) { lk.unlock(); FS.r_close(F); return false; }

    s_ready.emplace(key, F);
    s_parked += len;

    s_opened.fetch_add(1, std::memory_order_relaxed);
    s_openedB.fetch_add(len, std::memory_order_relaxed);
    return true;
}

// Lead file: one whole-file mapping shared by every worker, handed out in chunks
// by an atomic cursor so they sweep it front to back — the order its sequential
// reader wants.
//
// It warms the file's SECTION, and for a while that was assumed to be all the
// reader needed. It is not: a page resident in the cache still costs the reading
// thread a fault to put into ITS view's page table, and the geometry loader maps
// a fresh 1 MB window every megabyte. Measured with r_geom_prefault 1 — touching
// the window's own pages first costs 465 ms and leaves a copy that then runs at
// 13.7 GB/s instead of 5.9. So the loader stages out of THIS view instead (see
// LeadView / rvk_loader), and the faults stay where they already were: spread
// across sixteen workers, ahead of the read.
HANDLE           s_leadFile = INVALID_HANDLE_VALUE;
HANDLE           s_leadMap  = nullptr;
const u8*        s_leadBase = nullptr;
u64              s_leadSize = 0;
string_path      s_leadPath = {};
std::atomic<u64> s_leadNext{0};
std::atomic<u64> s_leadDone{0};

constexpr u64 kLeadChunk = 8u << 20;   // 8 MB: enough to keep an NVMe queue busy

void OpenLead(const char* path)
{
    s_leadFile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (s_leadFile == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(s_leadFile, &sz) || sz.QuadPart <= 0) { CloseHandle(s_leadFile); s_leadFile = INVALID_HANDLE_VALUE; return; }
    s_leadSize = (u64)sz.QuadPart;
    s_leadMap  = CreateFileMapping(s_leadFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!s_leadMap) { CloseHandle(s_leadFile); s_leadFile = INVALID_HANDLE_VALUE; s_leadSize = 0; return; }
    s_leadBase = (const u8*)MapViewOfFile(s_leadMap, FILE_MAP_READ, 0, 0, 0);
    if (!s_leadBase) { CloseHandle(s_leadMap); CloseHandle(s_leadFile); s_leadMap = nullptr; s_leadFile = INVALID_HANDLE_VALUE; s_leadSize = 0; return; }
    xr_strcpy(s_leadPath, path);
}

// Teardown thread: unmapping the lead view and closing the readers nobody claimed
// is pure release work that the level is waiting on for no reason. UnmapViewOfFile
// on a 1.9 GB fully-resident view tears down ~460k PTEs, and 51 unclaimed readers
// unmap and close two handles each — measured together as the tail of the visual
// phase, AFTER the join that the "walk waited" number covers. Joined at the next
// Start (and at the start of the next Stop), so it can never outlive the level.
// ⚠⚠...but nothing joined it at EXIT: the last level's teardown thread was still a
// joinable std::thread when the static destructors ran, and ~thread() on a joinable
// thread is std::terminate by the standard -- every quit to desktop ended in
// FATAL ERROR "_terminate" from this atexit destructor, long after the device was
// gone. The thread itself finished minutes earlier; it only had to be joined.
struct JoinOnExit
{
    std::thread t;
    JoinOnExit& operator=(std::thread&& other)
    {
        t = std::move(other);
        return *this;
    }
    bool joinable() const { return t.joinable(); }
    void join() { t.join(); }
    ~JoinOnExit()
    {
        if (t.joinable())
            t.join();
    }
} s_teardown;

void JoinTeardown()
{
    if (s_teardown.joinable()) s_teardown.join();
}

void CloseLead()
{
    if (s_leadBase) UnmapViewOfFile((void*)s_leadBase);
    if (s_leadMap)  CloseHandle(s_leadMap);
    if (s_leadFile != INVALID_HANDLE_VALUE) CloseHandle(s_leadFile);
    s_leadBase = nullptr; s_leadMap = nullptr; s_leadFile = INVALID_HANDLE_VALUE;
    s_leadSize = 0; s_leadNext = 0; s_leadPath[0] = 0;
}

// One job. With r_tex_materialize the worker OWNS it end to end -- read, image,
// upload, publish into the cache the walk will look in -- instead of parking the
// bytes and leaving the other four fifths of the work on the thread the level is
// waiting for. Returns false when the worker should stop.
bool DoJob(const TexJob& job)
{
    // 1 = build from the first job; 2 = read and park until the walk starts, then
    // build. The difference is which phase pays for the upload bus (see
    // EnableMaterialize).
    const bool build = (ps_r_tex_materialize == 1) ||
                       (ps_r_tex_materialize >= 2 && s_matOn.load(std::memory_order_relaxed));
    if (build && s_load) {
        // Optional throttle: building ends in the upload ring, which is one mutex, and
        // the copy that fills it wants cores of its own.
        const u32 cap = (u32)_max(0, ps_r_tex_mat_threads);
        if (cap) {
            std::unique_lock<std::mutex> lk(s_matMx);
            s_matCv.wait(lk, [cap] { return !s_running || s_matBusy < cap; });
            ++s_matBusy;
        }
        s_load(job);
        if (cap) {
            { std::lock_guard<std::mutex> lk(s_matMx); --s_matBusy; }
            s_matCv.notify_one();
        }
        s_materialized.fetch_add(1, std::memory_order_relaxed);
        return s_running;
    }
    return ParkOne(job.path);
}

void WorkerBody()
{
    t_TexWorker = true;
    for (u64 off = s_leadNext.fetch_add(kLeadChunk); s_leadBase && off < s_leadSize;
         off = s_leadNext.fetch_add(kLeadChunk)) {
        if (!s_running) return;
        const u64 end = _min(off + kLeadChunk, s_leadSize);
        u32 sink = 0;
        for (u64 o = off; o < end; o += 4096) sink += s_leadBase[o];
        s_touchSink.fetch_add(sink, std::memory_order_relaxed);
        s_leadDone.fetch_add(end - off, std::memory_order_relaxed);
    }

    xr_vector<TexJob> jobs;

    // Lightmaps FIRST (r_tex_prefetch_lmaps_first). They used to come last, and the
    // consequence was measured: the walk asks for them in its first milliseconds
    // (every lightmapped material needs one), the workers were still grinding
    // through 2400 diffuse bases, so all 27 missed — 432 MB the loading thread then
    // read itself, a page fault at a time, inside the BC3->BC4 gather. The workers
    // parked them afterwards for nobody: "unclaimed 51 (441 MB)" was 27 lightmaps
    // read twice. They are few, they are 16 MB each, and they are needed first.
    if (ps_r_tex_prefetch_lmaps_first)
        for (u32 i = s_nextDirect++; i < (u32)s_direct.size(); i = s_nextDirect++) {
            if (!s_running) return;
            if (!DoJob(s_direct[i])) return;
        }

    // Shader-table bases: each expands to that material's whole texture set,
    // which is exactly the group the walk asks for in one go.
    for (u32 i = s_nextName++; i < (u32)s_names.size(); i = s_nextName++) {
        if (!s_running) return;
        jobs.clear();
        if (s_expand) s_expand(s_names[i].c_str(), jobs);
        for (const TexJob& j : jobs)
            if (!DoJob(j)) return;
    }
    for (u32 i = s_nextDirect++; i < (u32)s_direct.size(); i = s_nextDirect++) {
        if (!s_running) return;
        if (!DoJob(s_direct[i])) return;
    }
}

CTimer s_wall;

}   // anonymous namespace

// The whole-file view of the lead file, for a caller that wants to read the same
// file the workers are prefaulting. Its own mapping would fault per 4 KB on its
// own thread; this one is being warmed by sixteen. Null unless `path` IS the lead.
const u8* LeadView(const char* path, u64& size)
{
    size = 0;
    if (!s_leadBase || !path || !path[0] || !s_leadPath[0]) return nullptr;
    if (_stricmp(path, s_leadPath) != 0) return nullptr;
    size = s_leadSize;
    return s_leadBase;
}

void Start(xr_vector<shared_str>&& diffuseNames, xr_vector<TexJob>&& directJobs, ExpandFn expand,
           LoadFn materialize, const char* leadFile)
{
    if (ps_r_tex_prefetch == 0) return;
    if (s_running) Stop();
    JoinTeardown();   // the previous level's unmap must be done before we map again
    if (diffuseNames.empty() && directJobs.empty() && !leadFile) return;

    if (leadFile && leadFile[0]) OpenLead(leadFile);


    s_names  = std::move(diffuseNames);
    s_direct = std::move(directJobs);
    s_expand = expand;
    s_load   = materialize;
    s_materialized = 0;
    s_matOn  = false;
    s_nextName = 0;
    s_nextDirect = 0;
    s_opened = 0; s_hits = 0; s_misses = 0; s_dropped = 0;
    s_openedB = 0; s_hitB = 0;
    s_parked = 0;
    s_budget = (u64)_max(64, ps_r_tex_prefetch_mb) << 20;
    s_running = true;
    s_wall.Start();

    u32 nThr = std::thread::hardware_concurrency();
    nThr = _min(_max(1u, nThr), 16u);
    s_threads = nThr;
    s_pool.reserve(nThr);
    for (u32 w = 0; w < nThr; ++w)
        s_pool.emplace_back([] { WorkerBody(); });

    Msg("[VK TexPrefetch] started: lead %u MB + %u shader bases + %u direct files on %u threads, budget %u MB, mode=%s",
        (u32)(s_leadSize >> 20), (u32)s_names.size(), (u32)s_direct.size(), nThr, (u32)(s_budget >> 20),
        (ps_r_tex_materialize && s_load) ? "materialize" : "park");

}

void CloseAsync(IReader*& F)
{
    if (!F) return;
    const u64 len = (u64)F->length();
    {
        std::lock_guard<std::mutex> lk(s_clMx);
        if (s_clBytes + len <= kCloseCapBytes) {
            if (s_clPool.empty()) {
                s_clStop = false;
                for (u32 i = 0; i < 2; ++i) s_clPool.emplace_back(CloseWorker);
            }
            s_clBytes += len;
            s_clQ.push_back(F);
            F = nullptr;
        }
    }
    if (!F) { s_clCv.notify_one(); return; }
    FS.r_close(F);   // over the cap: pay for it here, exactly as before
}

void CloseDrain()
{
    std::unique_lock<std::mutex> lk(s_clMx);
    s_clIdle.wait(lk, [] { return s_clQ.empty() && s_clBusy == 0; });
}

IReader* Take(const char* fullPath)
{
    if (!fullPath || !fullPath[0]) return nullptr;
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_ready.empty()) {
        // Nothing parked: either the prefetch is off, or it is behind. Only count a
        // miss while it is actually running, so unrelated (UI/model) loads don't
        // pollute the hit rate -- and never for a worker, which parked nothing for
        // itself and would report a 0% hit rate for a path with nothing to hit.
        if (s_running && !t_TexWorker) s_misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    auto it = s_ready.find(KeyOf(fullPath));
    if (it == s_ready.end()) {
        if (s_running && !t_TexWorker) s_misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    IReader* F = it->second;
    const u64 len = (u64)F->length();
    s_ready.erase(it);
    s_parked -= _min(s_parked, len);
    s_hits.fetch_add(1, std::memory_order_relaxed);
    s_hitB.fetch_add(len, std::memory_order_relaxed);
    s_cvSpace.notify_one();
    return F;
}

void EnableMaterialize()
{
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_matOn.store(true, std::memory_order_relaxed);
    }
    // Workers blocked on the parking budget have to wake up: from here they consume
    // what they parked instead of adding to it.
    s_cvSpace.notify_all();
}

void Stop()
{
    CTimer _tJoin; _tJoin.Start();
    JoinTeardown();   // a previous level's, if any — never two in flight
    CloseDrain();   // deferred closes belong to THIS level, not the next one
    if (!s_running && s_pool.empty()) return;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        s_running = false;
    }
    s_cvSpace.notify_all();
    for (auto& th : s_pool) if (th.joinable()) th.join();
    s_pool.clear();
    s_wallMs = s_wall.GetElapsed_ms_total();
    // How long the walk had to WAIT here: if the workers are still busy when the
    // visuals are done, the phase is bound by the prefetch tail, not by the walk.
    s_joinMs = _tJoin.GetElapsed_ms_total();

    u64 leftB = 0;
    xr_vector<IReader*> unclaimed;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        unclaimed.reserve(s_ready.size());
        // Name them. 441 MB read, decompressed and never asked for is not a rounding
        // error during the phase the whole load waits on -- but it is only worth
        // attacking if the names say WHICH rule produced them.
        u32 shown = 0;
        for (auto& kv : s_ready)
            if (shown++ < 64)
                Msg("[VK TexPrefetch] unclaimed: '%s' (%u KB)", kv.first.c_str(), (u32)(kv.second->length() >> 10));
        for (auto& kv : s_ready) { leftB += (u64)kv.second->length(); unclaimed.push_back(kv.second); }
        s_dropped = (u32)s_ready.size();
        s_ready.clear();
        s_produced.clear();
        s_parked = 0;
    }
    const u64 leadDone = s_leadDone.exchange(0);
    const u64 leadSize = s_leadSize;
    // Hand the release work to the teardown thread; the level does not wait for it.
    s_teardown = std::thread([unclaimed = std::move(unclaimed)] {
        for (IReader* F : unclaimed) FS.r_close(F);
        CloseLead();
    });
    s_names.clear();
    s_direct.clear();


    if (leadSize)
        Msg("[load step]   TexPrefetch lead: %llu of %llu MB prefaulted",
            (unsigned long long)(leadDone >> 20), (unsigned long long)(leadSize >> 20));
    const u32 hits = s_hits.load(), misses = s_misses.load();

    Msg("[load step]   TexPrefetch: %u opened (%llu MB) on %u threads in %.0f ms wall (walk waited %.0f ms here) | walk hits %u / miss %u (%.0f%%) | unclaimed %u (%llu MB) | materialized %u",
        s_opened.load(), (unsigned long long)(s_openedB.load() >> 20), s_threads, s_wallMs, s_joinMs,
        hits, misses, (hits + misses) ? 100.0 * hits / double(hits + misses) : 0.0,
        s_dropped.load(), (unsigned long long)(leftB >> 20), s_materialized.load());
}

}   // namespace TexPrefetch

CVulkanTexture::DDSLoadResult CVulkanTexture::loadDDSToImage(const char* filename,
                                                            bool applyBCSwizzle, u32 mipSkip,
                                                            TexColorSpace colorSpace)
{
    VK::TexLoadProf::scope _profAll(VK::TexLoadProf::s_totalClk);
    ++VK::TexLoadProf::s_count;

    const u64 _cOpen0 = CPU::GetCLK();
    // Already opened by a prefetch worker? Then this costs a hash lookup instead of
    // a whole-file LZO decompress. Ownership transfers — the r_close below is the
    // same one either way (see VK::TexPrefetch).
    IReader* F;
    {
        LoadProf::Scope _lp("tex.read");
        const u64 t0 = CPU::QPC();
        F = VK::TexPrefetch::Take(filename);
        if (!F) F = FS.r_open(filename);
        // [tex late]: 📏08-10-2026 Switch v237 tex.read 10-11 ms inside the render thread's record (next to
        // Wallmarks) in a fight -- a texture first needed in gameplay, read from the SD synchronously. Which one.
        const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());
        // 11-10: the thread too -- a read on a background loader is harmless, one on the render thread (or the main
        // thread) holds the frame: rt_wait 180-590 ms stalls in fights came next to these lines. Those get a call chain.
        static std::atomic<u32> s_late{0};
        if (ms >= 3.0 && !Device.dwPrecacheFrame && g_pGameLevel && s_late.fetch_add(1, std::memory_order_relaxed) < 64)
        {
            const bool rt = RT::IsRenderThread(), main = !rt && Device.OnMainThread();
            Msg("~ [tex late] '%s' read %.1f ms in gameplay (frame %u) on the %s thread", filename, ms, Device.dwFrame,
                rt ? "RENDER" : main ? "MAIN" : "background");
#ifdef __SWITCH__
            if ((rt || main) && ms >= 5.0)
                xr_nx_backtrace("tex late");
#endif
        }
    }
    if (!F) {
        Msg("![Vulkan] Failed to open texture: %s", filename);
        return {};
    }

    // Archived files are decompressed whole here — this is the read cost a mip
    // cap can NOT reduce, so it is measured on its own.
    const size_t _len = (size_t)F->length();
    const void*  _ptr = F->pointer();
    VK::TexLoadProf::s_openClk   += CPU::GetCLK() - _cOpen0;
    VK::TexLoadProf::s_bytesRead += _len;

    // IReader is memory-backed (mapped or decompressed) — parse in place.
    DDSLoadResult r;
    {
        LoadProf::Scope _lp("tex.parse");
        r = loadDDSFromMemory(filename, _ptr, _len,
                              applyBCSwizzle, mipSkip, colorSpace);
    }
    {
        LoadProf::Scope _lp("tex.close");
        VK::TexLoadProf::scope _profClose(VK::TexLoadProf::s_closeClk);
        VK::TexPrefetch::CloseAsync(F);   // archived: frees the decompressed image; loose: unmaps it
    }
    return r;
}

CVulkanTexture::DDSLoadResult CVulkanTexture::loadDDSFromMemory(const char* filename,
                                                                const void* blob, size_t blobSize,
                                                                bool applyBCSwizzle, u32 mipSkip,
                                                                TexColorSpace colorSpace)
{
    // Header parse + format pick + the truncation walk: cheap per texture, 2463 of
    // them per load, and never on the books until now.
    const u64 _cParse0 = CPU::GetCLK();

    const u8* cur = (const u8*)blob;
    size_t    rem = blob ? blobSize : 0;
    auto take = [&](void* dst, size_t n) -> bool {
        if (rem < n) return false;
        memcpy(dst, cur, n); cur += n; rem -= n;
        return true;
    };

    // Check magic
    u32 magic = 0;
    if (!take(&magic, 4) || magic != DDS_MAGIC) {
        Msg("![Vulkan] Invalid DDS magic in %s", filename);
        return {};
    }

    // Read header
    DDS_HEADER header;
    if (!take(&header, sizeof(DDS_HEADER))) {
        Msg("![Vulkan] Truncated DDS header in %s", filename);
        return {};
    }

    // Determine format
    VkFormat format = VK_FORMAT_UNDEFINED;
    bool expand24to32 = false;   // set for 24-bit RGB → upconverted to 32-bit on load

    if (header.ddspf.dwFlags & DDPF_FOURCC) {
        if (header.ddspf.dwFourCC == FOURCC_DX10) {
            // Extended DX10 header carries an explicit DXGI format (BC7/BC6H etc.).
            // Reading it also advances past the 20 extra bytes so the pixel data
            // that follows is at the correct file offset.
            DDS_HEADER_DXT10 h10{};
            if (!take(&h10, sizeof(h10))) {
                Msg("![Vulkan] Truncated DX10 header in %s", filename);
                return {};
            }
            format = DXGIFormatToVk(h10.dxgiFormat);
            if (format == VK_FORMAT_UNDEFINED) {
                Msg("![Vulkan] Unsupported DXGI format %u (DX10 header) in %s", h10.dxgiFormat, filename);
                return {};
            }
            // The legacy R↔B swap only applies to BGR-ordered DXT UI atlases;
            // DX10 textures encode their true channel order in dxgiFormat, so swap
            // only when the caller asked AND the format is a (BC) compressed one.
            m_bBCSwizzle = applyBCSwizzle && IsCompressedFormat(format);
        } else {
            format = FourCCToVkFormat(header.ddspf.dwFourCC);
            if (format == VK_FORMAT_UNDEFINED) {
                Msg("![Vulkan] Unsupported FourCC: %X in %s", header.ddspf.dwFourCC, filename);
                return {};
            }
            // X-Ray UI atlases ship with BGR-ordered BC endpoints (yellow indicators
            // come out blue without R↔B swap). Level statics are stock BC1/3 with
            // RGB endpoints — `applyBCSwizzle=false` keeps them correct.
            m_bBCSwizzle = applyBCSwizzle;
        }
    } else if (header.ddspf.dwFlags & DDPF_RGB) {
        if (header.ddspf.dwRGBBitCount == 32) {
            // Choose format based on channel masks. D3DFMT_A8R8G8B8 (BGRA-in-
            // memory) uses R=0x00FF0000; D3DFMT_A8B8G8R8 (RGBA-in-memory) uses
            // R=0x000000FF. X-Ray ships UI atlases in the latter convention
            // (caught when ui_common indicators rendered with R↔B swapped).
            if (header.ddspf.dwRBitMask == 0x000000FF) {
                format = VK_FORMAT_R8G8B8A8_UNORM;
            } else {
                format = VK_FORMAT_B8G8R8A8_UNORM;
            }
        } else if (header.ddspf.dwRGBBitCount == 24) {
            // Vulkan optimal-tiling sampled images don't support 3-byte R8G8B8, so
            // we upconvert to 32-bit (opaque alpha) on load. The blue mask tells us
            // the in-memory byte order (B first → D3DFMT_R8G8B8 / BGR, the common
            // case), so we tag the matching 32-bit format and copy bytes straight
            // through in the expansion pass below — no channel shuffle needed.
            format = (header.ddspf.dwBBitMask == 0x000000FF)
                       ? VK_FORMAT_B8G8R8A8_UNORM
                       : VK_FORMAT_R8G8B8A8_UNORM;
            expand24to32 = true;
        } else {
            Msg("![Vulkan] Unsupported RGB bit count: %d in %s", header.ddspf.dwRGBBitCount, filename);
            return {};
        }
    } else if (header.ddspf.dwFlags & DDPF_ALPHA) {
        // Alpha-only format (A8) — used by font textures
        // Store as R8, swizzle R→A in image view
        format = VK_FORMAT_R8_UNORM;
        m_bAlphaSwizzle = true;
    } else if (header.ddspf.dwFlags & DDPF_LUMINANCE) {
        // Luminance format (L8 or L8A8)
        if (header.ddspf.dwRGBBitCount == 8) {
            format = VK_FORMAT_R8_UNORM;
        } else if (header.ddspf.dwRGBBitCount == 16) {
            format = VK_FORMAT_R8G8_UNORM;
        } else {
            Msg("![Vulkan] Unsupported luminance bit count: %d in %s", header.ddspf.dwRGBBitCount, filename);
            return {};
        }
    } else {
        Msg("![Vulkan] Unsupported DDS format flags: %X in %s", header.ddspf.dwFlags, filename);
        return {};
    }

    // LINEAR PIPELINE (r_linear_color): the single point where "this texture holds
    // colour" turns into "the sampler decodes sRGB for us". Every branch above picked a
    // UNORM format — historically ALL of them did, unconditionally, which is exactly why
    // the renderer shaded on gamma-encoded albedo (see the display-encode note in
    // vk_pass_tonemap.cpp). Flipping the format here is enough: no shader that samples a
    // Colour texture needs to change, because the hardware does the decode on fetch.
    //
    // Gated so the old all-gamma pipeline stays reachable for A/B. The colourspace is
    // baked into the image format at LOAD time, so the selector is the process-latched
    // ColorSpace::Active() — a mid-session cvar flip would otherwise leave resident and
    // streamed-in textures in different spaces (and the OETF out of step with both).
    if (colorSpace == TexColorSpace::Color && VK::ColorSpace::Active())
        format = ToSrgbFormat(format);

    u32 width = header.dwWidth;
    u32 height = header.dwHeight;
    u32 mipLevels = (header.dwFlags & 0x20000) ? header.dwMipMapCount : 1; // DDSD_MIPMAPCOUNT
    if (mipLevels == 0) mipLevels = 1;

    // On-disk full-chain metadata — captured BEFORE any mip skip so the streamer
    // knows how far a texture could still be promoted.
    const u32 fullW    = width;
    const u32 fullH    = height;
    const u32 fullMips = mipLevels;

    // Remaining bytes = the whole mip chain, still inside the caller's blob. No
    // copy — UploadData memcpy's into the staging ring itself. `ownsData` flips
    // only when the 24→32 expansion below rebuilds the chain in a fresh buffer.
    VkDeviceSize dataSize = (VkDeviceSize)rem;
    const void*  data     = cur;
    bool         ownsData = false;

    // 24-bit RGB → 32-bit RGBA expansion. Walk the mip chain exactly as
    // UploadData does (halve per level, floor at 1) so the rebuilt buffer is
    // tightly packed at 4 bpp and the copy regions line up.
    if (expand24to32) {
        u32 dstSize = 0;
        for (u32 w = width, h = height, i = 0; i < mipLevels; ++i) {
            dstSize += w * h * 4;
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
        u8* dst = (u8*)xr_malloc(dstSize);
        const u8* src = (const u8*)data;
        u8* d = dst;
        for (u32 w = width, h = height, i = 0; i < mipLevels; ++i) {
            const u32 px = w * h;
            for (u32 p = 0; p < px; ++p) {
                d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = 0xFF;
                d += 4; src += 3;
            }
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
        data     = dst;
        dataSize = dstSize;
        ownsData = true;
    }

    // Guard against a DDS whose header over-claims its contents (truncated file
    // or a wrong dwMipMapCount — seen on some level-packed textures reached via
    // the $level$ fallback). UploadData walks the full mip chain to build copy
    // regions; if those reference more bytes than we actually read, the driver's
    // vkCmdCopyBufferToImage reads past the staging buffer → GPU/driver crash
    // (was: hard crash mid-game when such an NPC/level texture streamed in).
    // Mirror UploadData's exact size walk; on shortfall, fail the load so the
    // caller falls back to the white default instead of feeding the driver.
    {
        VkDeviceSize expected = 0;
        for (u32 w = width, h = height, i = 0; i < mipLevels; ++i) {
            if (IsCompressedFormat(format))
                expected += (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * GetBlockSize(format);
            else {
                const u32 bpp = (format == VK_FORMAT_R8_UNORM) ? 1u : (format == VK_FORMAT_R8G8_UNORM) ? 2u : 4u;
                expected += (VkDeviceSize)w * h * bpp;
            }
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
        if (expected > dataSize) {
            Msg("![Vulkan] DDS truncated/corrupt: '%s' needs %llu bytes (%ux%u mips=%u) but only %llu present — skipping (white default)",
                filename, (unsigned long long)expected, fullW, fullH, fullMips, (unsigned long long)dataSize);
            if (ownsData) { void* p = const_cast<void*>(data); xr_free(p); }
            return {};
        }
    }

    VK::TexLoadProf::s_parseClk += CPU::GetCLK() - _cParse0;


    // Hemi lightmaps only: which channel carries the bake (see DetectLmapHemiChannel),
    // and repack to single-channel BC4 while we hold the blob. Done here — after the
    // truncation guard (so the source is known complete), before the residency plan
    // and CreateView, both of which must see the FINAL format.
    bool repackedHere = false;
    {
        VK::TexLoadProf::scope _profRepack(VK::TexLoadProf::s_repackClk);
        // A hemi lightmap qualifies only if the bake actually sits in alpha (measured);
        // a height map qualifies by construction — every sampler of it reads .a.
        double rgbMean = 0.0, aMean = 0.0;
        const bool lmapInAlpha = (m_StreamClass == VK::TexStreamClass::Lmap)
                              && DetectLmapHemiChannel(data, dataSize, width, height, format, rgbMean, aMean);
        const bool heightOnly  = m_AlphaOnly
                              && (format == VK_FORMAT_BC3_UNORM_BLOCK || format == VK_FORMAT_BC3_SRGB_BLOCK);

        // TEXTURE AUDIT (r_tex_audit) — HERE, after the shipping gate is decided and
        // before the transcode runs. Both halves of that placement matter: after the
        // repack the format would read BC4 and hide what the audit exists to count,
        // and without knowing (lmapInAlpha || heightOnly) the report would bill the
        // lightmap saving the engine ALREADY takes as a new opportunity — which is
        // exactly what the first run of this instrument did (448 MB of "prize" that
        // was the hemi maps being repacked as designed).
        if (ps_r_tex_audit > 0)
            TexAudit::Note(format, width, height, dataSize, data, dataSize, lmapInAlpha || heightOnly);

        if (lmapInAlpha || heightOnly) {
            m_ChanFix = lmapInAlpha ? ChanFix::HemiFromAlpha : ChanFix::None;   // pre-repack fallbacks
            VkDeviceSize packedSize = 0;
            bool packedOwned = true;
            if (void* packed = TranscodeBC3AlphaToBC4(data, width, height, mipLevels, packedSize, packedOwned)) {
                VK::TexLoadProf::s_repackBytes += dataSize;   // source bytes, before the swap below
                if (ownsData) { void* old = const_cast<void*>(data); xr_free(old); }
                Msg("[VK %s] '%s': %s -> repacked BC3->BC4, %.1f -> %.1f MB",
                    lmapInAlpha ? "Lmap" : "Height", filename,
                    lmapInAlpha ? "hemi in ALPHA, rgb empty" : "alpha-only sampled, rgb unused",
                    dataSize / 1048576.0, packedSize / 1048576.0);
                data      = packed;
                dataSize  = packedSize;
                ownsData  = packedOwned;   // false when the gather wrote into the reused scratch
                ++VK::TexLoadProf::s_repacked;
                repackedHere = true;
                format    = VK_FORMAT_BC4_UNORM_BLOCK;
                // BC4 delivers the scalar in R; route it to wherever shaders look.
                m_ChanFix = lmapInAlpha ? ChanFix::HemiFromRed : ChanFix::AlphaFromRed;
            } else if (lmapInAlpha) {
                Msg("[VK Lmap] '%s': hemi in ALPHA (rgb %.3f empty) — repack allocation failed, sampling alpha in place",
                    filename, rgbMean);
            }
        }
    }

    // ---- Residency: skip the top `skip` mips (quality slider / budget-fit / stream).
    u32 skip = (mipSkip == UINT32_MAX)
                 ? VK::TextureStreamer::Instance().PlanLoadMipSkip(fullW, fullH, fullMips, format, m_StreamClass)
                 : mipSkip;
    if (mipLevels <= 1)                skip = 0;
    else if (skip > mipLevels - 1)     skip = mipLevels - 1;
    // Defensive floor: never let the base mip fall below the format's block/pixel min.
    {
        const u32 minDim = IsCompressedFormat(format) ? 4u : 1u;
        while (skip > 0) {
            const u32 bw = (width  >> skip) ? (width  >> skip) : 1u;
            const u32 bh = (height >> skip) ? (height >> skip) : 1u;
            if (bw >= minDim && bh >= minDim) break;
            --skip;
        }
    }

    VkDeviceSize skipBytes = 0;
    if (skip > 0) {
        for (u32 w = width, h = height, i = 0; i < skip; ++i) {
            if (IsCompressedFormat(format))
                skipBytes += (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * GetBlockSize(format);
            else {
                const u32 bpp = (format == VK_FORMAT_R8_UNORM) ? 1u : (format == VK_FORMAT_R8G8_UNORM) ? 2u : 4u;
                skipBytes += (VkDeviceSize)w * h * bpp;
            }
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
        }
        for (u32 i = 0; i < skip; ++i) { if (width > 1) width >>= 1; if (height > 1) height >>= 1; }
        mipLevels -= skip;
    }
    // Bytes the repack produced and the residency plan then dropped. The repack has
    // to run first (the plan needs the FINAL format), but it does not have to gather
    // mips nobody will upload -- if this number is large.
    if (repackedHere) VK::TexLoadProf::s_repackSkipBytes += skipBytes;

    // Debug: log format for magnifier texture specifically
    if (strstr(filename, "magnifier")) {
        Msg("[Vulkan Texture] MAGNIFIER DETAILED: %s %ux%u mips=%u (skip=%u) fmt=%d flags=0x%X fourcc=0x%X",
            filename, width, height, mipLevels, skip, (int)format, header.dwFlags, header.ddspf.dwFourCC);
    }

    // Create texture at the resident dimensions.
    {
        LoadProf::Scope _lp("tex.create");
        VK::TexLoadProf::scope _profCreate(VK::TexLoadProf::s_createClk);
        Create(width, height, format, mipLevels);
    }
    if (m_Image == VK_NULL_HANDLE) {   // allocation failed (OOM) — bail cleanly
        if (ownsData) { void* p = const_cast<void*>(data); xr_free(p); }
        return {};
    }
    // Tag the VMA allocation with the source file so any leaked image is
    // identifiable by name in the vmaDestroyAllocator leak dump (see vma_impl.cpp).
    if (m_Allocation) vmaSetAllocationName(VulkanHW.m_Allocator, m_Allocation, filename);

    // Upload from the first resident mip onward (offset past the skipped mips).
    if (m_DeferUpload) {
        // PrebuildStreamImageFromBlob: the copy is staged later by FinishDeferredUpload, on the thread that owns the
        // upload batch. Keep the pointer (into the caller's blob, or into the repacked copy we now own).
        m_DeferredData  = (const u8*)data + skipBytes;
        m_DeferredSize  = dataSize - skipBytes;
        m_DeferredOwned = ownsData ? const_cast<void*>(data) : nullptr;
    } else {
        {
            LoadProf::Scope _lp("tex.upload");
            VK::TexLoadProf::scope _profUpload(VK::TexLoadProf::s_uploadClk);
            UploadData((const u8*)data + skipBytes, dataSize - skipBytes);
            VK::TexLoadProf::s_bytesUploaded += (dataSize - skipBytes);
        }
        if (ownsData) { void* p = const_cast<void*>(data); xr_free(p); }
    }

    DDSLoadResult res;
    res.ok            = true;
    res.fullW         = fullW;
    res.fullH         = fullH;
    res.fullMips      = fullMips;
    res.format        = format;
    res.residentBase  = skip;
    res.residentBytes = VK::TextureStreamer::MipChainBytes(fullW, fullH, fullMips, skip, format);
    return res;
}

// Public DDS load: resolve residency via the streamer plan, then register so the
// texture participates in budget accounting + (optionally) dynamic streaming.
bool CVulkanTexture::LoadDDS(const char* filename, bool applyBCSwizzle, TexStreamClass streamClass,
                             TexColorSpace colorSpace)
{
    m_StreamClass       = streamClass;
    m_LoadSwizzleIntent = applyBCSwizzle;
    m_LoadColorSpace    = colorSpace;
    m_MemPriority       = PriorityForClass(streamClass);

    DDSLoadResult r = loadDDSToImage(filename, applyBCSwizzle, UINT32_MAX, colorSpace);
    if (!r.ok)
        return false;

    m_SourceFile = filename;
    VK::TextureStreamer::Instance().Register(this, filename, r.fullW, r.fullH, r.fullMips,
                                             r.format, r.residentBase, r.residentBytes, streamClass);
    m_Registered = true;
    return true;
}

// Build a NEW image for `out` covering mips [mipSkip..end] from THIS texture's source
// .dds, for a streaming promote/demote. `out` must be empty; it is NOT registered.
bool CVulkanTexture::BuildStreamImage(CVulkanTexture& out, u32 mipSkip) const
{
    if (m_SourceFile.size() == 0) return false;
    out.m_StreamClass       = m_StreamClass;
    out.m_LoadSwizzleIntent = m_LoadSwizzleIntent;
    out.m_AlphaOnly         = m_AlphaOnly;   // else a rebuilt mip would skip the BC4 repack
    // Carry the colourspace across a promote/demote. Without this the rebuilt image
    // would re-derive its format from the default (Data → UNORM) and a texture would
    // silently change colourspace mid-session the first time it streamed a mip.
    out.m_LoadColorSpace    = m_LoadColorSpace;
    out.m_MemPriority       = m_MemPriority;
    DDSLoadResult r = out.loadDDSToImage(m_SourceFile.c_str(), m_LoadSwizzleIntent, mipSkip,
                                         m_LoadColorSpace);
    return r.ok;
}

// Async-IO variant: the .dds was already read (off-thread) into `blob`.
bool CVulkanTexture::BuildStreamImageFromBlob(CVulkanTexture& out, u32 mipSkip,
                                              const void* blob, size_t blobSize) const
{
    if (m_SourceFile.size() == 0 || !blob || blobSize == 0) return false;
    out.m_StreamClass       = m_StreamClass;
    out.m_LoadSwizzleIntent = m_LoadSwizzleIntent;
    out.m_AlphaOnly         = m_AlphaOnly;   // else a rebuilt mip would skip the BC4 repack
    out.m_LoadColorSpace    = m_LoadColorSpace;   // see BuildStreamImage
    out.m_MemPriority       = m_MemPriority;
    DDSLoadResult r = out.loadDDSFromMemory(m_SourceFile.c_str(), blob, blobSize,
                                            m_LoadSwizzleIntent, mipSkip, m_LoadColorSpace);
    return r.ok;
}

CVulkanTexture::StreamBuildParams CVulkanTexture::GetStreamBuildParams() const
{
    StreamBuildParams p;
    p.file       = m_SourceFile.size() ? m_SourceFile.c_str() : "";
    p.cls        = m_StreamClass;
    p.swizzle    = m_LoadSwizzleIntent;
    p.alphaOnly  = m_AlphaOnly;
    p.colorSpace = m_LoadColorSpace;
    p.priority   = m_MemPriority;
    return p;
}

bool CVulkanTexture::PrebuildStreamImageFromBlob(CVulkanTexture& out, const StreamBuildParams& src, u32 mipSkip,
                                                 const void* blob, size_t blobSize)
{
    if (src.file.empty() || !blob || blobSize == 0) return false;
    out.m_StreamClass       = src.cls;
    out.m_LoadSwizzleIntent = src.swizzle;
    out.m_AlphaOnly         = src.alphaOnly;
    out.m_LoadColorSpace    = src.colorSpace;
    out.m_MemPriority       = src.priority;
    out.m_DeferUpload       = true;
    DDSLoadResult r = out.loadDDSFromMemory(src.file.c_str(), blob, blobSize, src.swizzle, mipSkip, src.colorSpace);
    out.m_DeferUpload = false;
    return r.ok && out.m_DeferredData != nullptr;
}

void CVulkanTexture::FinishDeferredUpload()
{
    if (!m_DeferredData) return;
    {
        VK::TexLoadProf::scope _profUpload(VK::TexLoadProf::s_uploadClk);
        UploadData(m_DeferredData, m_DeferredSize);
        VK::TexLoadProf::s_bytesUploaded += m_DeferredSize;
    }
    if (m_DeferredOwned) { xr_free(m_DeferredOwned); m_DeferredOwned = nullptr; }
    m_DeferredData = nullptr;
    m_DeferredSize = 0;
}

// Exchange every GPU handle + descriptor-visible field with `other`. Streaming
// metadata (source file, class, registration) stays with each object so the live
// texture keeps its streamer identity while the old handles migrate into `other`
// for deferred destruction. See TextureStreamer::StreamStep.
void CVulkanTexture::SwapContents(CVulkanTexture& other)
{
    std::swap(m_Image,        other.m_Image);
    std::swap(m_Allocation,   other.m_Allocation);
    std::swap(m_ImageView,    other.m_ImageView);
    std::swap(m_Sampler,      other.m_Sampler);
    std::swap(m_Width,        other.m_Width);
    std::swap(m_Height,       other.m_Height);
    std::swap(m_MipLevels,    other.m_MipLevels);
    std::swap(m_Format,       other.m_Format);
    std::swap(m_CurrentLayout,other.m_CurrentLayout);
    std::swap(m_bAlphaSwizzle,other.m_bAlphaSwizzle);
    std::swap(m_bBCSwizzle,   other.m_bBCSwizzle);
    std::swap(m_ChanFix,      other.m_ChanFix);        // view-visible: must migrate with the handles
    std::swap(m_bCubemap,     other.m_bCubemap);
    std::swap(m_ArrayLayers,  other.m_ArrayLayers);
    std::swap(m_MemPriority,  other.m_MemPriority);
}

// Загрузка DDS cubemap (6 faces)
bool CVulkanTexture::LoadDDSCubemap(const char* filename, bool applyBCSwizzle,
                                    TexColorSpace colorSpace)
{
    IReader* F = FS.r_open(filename);
    if (!F) {
        Msg("![Vulkan] Failed to open cubemap texture: %s", filename);
        return false;
    }

    // Check magic
    u32 magic = 0;
    F->r(&magic, 4);
    if (magic != DDS_MAGIC) {
        Msg("![Vulkan] Invalid DDS magic in cubemap %s", filename);
        FS.r_close(F);
        return false;
    }

    // Read header
    DDS_HEADER header;
    F->r(&header, sizeof(DDS_HEADER));

    // Check for cubemap flag
    const u32 DDSCAPS2_CUBEMAP = 0x200;
    if (!(header.dwCaps2 & DDSCAPS2_CUBEMAP)) {
        Msg("![Vulkan] DDS file is not a cubemap: %s (caps2=0x%X)", filename, header.dwCaps2);
        FS.r_close(F);
        return false;
    }

    // Determine format. Historically this was hard-wired to UNORM ("swapchain is UNORM,
    // no sRGB conversion in the pipeline — matches D3D11/R4 where textures stay in gamma
    // space throughout"), which is why the sky fed gamma-encoded radiance into the sky
    // ambient/IBL maths. Under r_linear_color a Colour cubemap now picks the _SRGB twin
    // below, exactly like the 2D path. NOTE: this loader is a SEPARATE copy of the format
    // logic — it never calls loadDDSFromMemory/DXGIFormatToVk, so it needs its own flip.
    VkFormat format = VK_FORMAT_UNDEFINED;

    if (header.ddspf.dwFlags & DDPF_FOURCC) {
        format = FourCCToVkFormat(header.ddspf.dwFourCC);
        if (format == VK_FORMAT_UNDEFINED) {
            Msg("![Vulkan] Unsupported cubemap FourCC: %X in %s", header.ddspf.dwFourCC, filename);
            FS.r_close(F);
            return false;
        }
        // Same R↔B swizzle policy as 2D BC: caller decides.
        m_bBCSwizzle = applyBCSwizzle;
    } else if (header.ddspf.dwFlags & DDPF_RGB) {
        if (header.ddspf.dwRGBBitCount == 32) {
            format = VK_FORMAT_B8G8R8A8_UNORM;
        } else {
            Msg("![Vulkan] Unsupported cubemap RGB bit count: %d in %s", header.ddspf.dwRGBBitCount, filename);
            FS.r_close(F);
            return false;
        }
    } else {
        Msg("![Vulkan] Unsupported cubemap DDS format flags: %X in %s", header.ddspf.dwFlags, filename);
        FS.r_close(F);
        return false;
    }

    if (colorSpace == TexColorSpace::Color && VK::ColorSpace::Active())
        format = ToSrgbFormat(format);

    u32 width = header.dwWidth;
    u32 height = header.dwHeight;
    u32 srcMips = (header.dwFlags & 0x20000) ? header.dwMipMapCount : 1;
    if (srcMips == 0) srcMips = 1;

    // Sky cubemaps ship single-mip + uncompressed (BGRA8). The hemisphere sky
    // ambient (vk_env_light + world shaders) samples a blurred HIGH mip as a
    // diffuse-irradiance approximation (R4 hmodel.h CUBE_MIPS), so build the
    // full chain at load. Compressed cubes can't be linear-blit-filtered — keep
    // those single-mip (rare; ambient just samples sharper there).
    const bool genMips   = !IsCompressedFormat(format) && width > 1 && srcMips == 1;
    const u32  totalMips = genMips ? CalculateMipLevels(width, height) : srcMips;

    // Setup cubemap flags before Create()
    m_bCubemap = true;
    m_ArrayLayers = 6;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (genMips) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // blit source for mip-gen

    Create(width, height, format, totalMips, usage);
    if (m_Image == VK_NULL_HANDLE) {
        FS.r_close(F);
        return false;
    }
    if (m_Allocation) vmaSetAllocationName(VulkanHW.m_Allocator, m_Allocation, filename);  // leak-dump name

    // Read remaining data (6 faces; mip0 only when srcMips==1).
    VkDeviceSize dataSize = F->length() - F->tell();
    void* data = xr_malloc(dataSize);
    F->r(data, dataSize);
    FS.r_close(F);

    if (genMips) GenerateMipsCube(data, dataSize);   // synchronous staging copy + blit chain
    else         UploadData(data, dataSize);
    xr_free(data);

    Msg("[Vulkan] Loaded cubemap DDS: %s (%dx%d, mips=%d, format=%d, caps2=0x%X, dataSize=%llu)",
        filename, width, height, totalMips, (int)format, header.dwCaps2, (unsigned long long)dataSize);
    return true;
}

// Build the full mip chain for a cubemap from mip-0 face data — synchronous.
// The image is created with the full mip count + TRANSFER_SRC; here we copy the
// 6 mip-0 faces from a staging buffer then vkCmdBlitImage each level into the
// next (all 6 layers per blit). One immediate cmd buffer, fence-waited, so it's
// safe to call mid-frame (sky cube loads happen at weather boundaries, rare).
void CVulkanTexture::GenerateMipsCube(const void* mip0Data, VkDeviceSize mip0Size)
{
    CVulkanBuffer staging;
    staging.Create(mip0Size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST);
    void* p = staging.Map();
    if (!p) { Msg("![Vulkan] GenerateMipsCube: staging map failed"); staging.Destroy(); return; }
    memcpy(p, mip0Data, mip0Size);

    const VkDeviceSize faceSize = mip0Size / 6;   // tightly-packed mip0, 6 faces

    VkCommandBuffer cmd = CommandManager.BeginImmediate();
    if (cmd == VK_NULL_HANDLE) { staging.Destroy(); return; }

    auto barrier = [&](u32 baseMip, u32 mipCount, VkImageLayout oldL, VkImageLayout newL,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = m_Image;
        b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 6 };
        b.srcAccessMask       = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    // Whole image UNDEFINED → TRANSFER_DST, then copy the 6 mip-0 faces.
    barrier(0, m_MipLevels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy regions[6]{};
    for (u32 f = 0; f < 6; ++f) {
        regions[f].bufferOffset      = f * faceSize;
        regions[f].imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1 };
        regions[f].imageExtent       = { m_Width, m_Height, 1 };
    }
    vkCmdCopyBufferToImage(cmd, staging.GetHandle(), m_Image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);

    // mip0 → TRANSFER_SRC (blit source for mip1).
    barrier(0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    s32 mw = (s32)m_Width, mh = (s32)m_Height;
    for (u32 i = 1; i < m_MipLevels; ++i) {
        const s32 nw = mw > 1 ? mw / 2 : 1;
        const s32 nh = mh > 1 ? mh / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 6 };
        blit.srcOffsets[1]  = { mw, mh, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 6 };
        blit.dstOffsets[1]  = { nw, nh, 1 };
        vkCmdBlitImage(cmd, m_Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        // This level becomes the next blit's source.
        barrier(i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        mw = nw; mh = nh;
    }

    // All mips are TRANSFER_SRC now → SHADER_READ for the samplers.
    barrier(0, m_MipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    CommandManager.EndAndSubmitImmediate(cmd);   // fence-waited
    staging.Destroy();
    m_CurrentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// Рассчет количества mip levels
u32 CVulkanTexture::CalculateMipLevels(u32 width, u32 height)
{
    u32 mipLevels = 1;
    u32 maxDim = (width > height) ? width : height;
    while (maxDim > 1) {
        maxDim >>= 1;
        mipLevels++;
    }
    return mipLevels;
}

// Проверка compressed формата
bool CVulkanTexture::IsCompressedFormat(VkFormat format)
{
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return true;
        default:
            return false;
    }
}

// Размер блока для compressed формата
u32 CVulkanTexture::GetBlockSize(VkFormat format)
{
    switch (format) {
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return 8;  // 8 bytes per 4x4 block
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return 16; // 16 bytes per 4x4 block
        default:
            return 0;
    }
}

//------------------------------------------------------------------------------
// CUserTextureRegistry - $user$ texture system (Phase 0.3)
//------------------------------------------------------------------------------

void CUserTextureRegistry::Register(const char* name, CRT* rt)
{
    if (!name || !rt) {
        Msg("![Vulkan] Cannot register null user texture");
        return;
    }

    // Проверяем что имя начинается с $user$
    if (strncmp(name, "$user$", 6) != 0) {
        Msg("![Vulkan] User texture name must start with $user$: %s", name);
        return;
    }

    shared_str key = name;

    // Проверяем дубликаты
    auto it = m_UserTextures.find(key);
    if (it != m_UserTextures.end()) {
        Msg("~[Vulkan] User texture %s already registered, replacing", name);
    }

    m_UserTextures[key] = rt;
    Msg("[Vulkan] User texture registered: %s", name);
}

CRT* CUserTextureRegistry::Get(const char* name)
{
    if (!name) {
        return nullptr;
    }

    shared_str key = name;
    auto it = m_UserTextures.find(key);
    if (it != m_UserTextures.end()) {
        return it->second;
    }

    // Msg("~[Vulkan] User texture not found: %s", name);
    return nullptr;
}

void CUserTextureRegistry::Unregister(const char* name)
{
    if (!name) {
        return;
    }

    shared_str key = name;
    auto it = m_UserTextures.find(key);
    if (it != m_UserTextures.end()) {
        m_UserTextures.erase(it);
        Msg("[Vulkan] User texture unregistered: %s", name);
    }
}

void CUserTextureRegistry::Clear()
{
    m_UserTextures.clear();
    Msg("[Vulkan] All user textures cleared");
}

// Глобальный instance
CUserTextureRegistry g_UserTextureRegistry;

} // namespace VK
