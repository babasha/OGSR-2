// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "../../xrCore/load_prof.h" // [rt prof]: named parts of a slow render-thread job
#include "vk_swapchain.h"
#include "HW_Vulkan.h"
#include "vk_profiler.h"   // VK::Prof::DumpCheckpoints — GPU-hang post-mortem
#include "vk_framegraph.h" // VK::g_FrameGraph.Forget — drop layout entries on recreate

// Глобальный экземпляр
CVulkanSwapchain Swapchain;

// Выбор формата surface
VkSurfaceFormatKHR CVulkanSwapchain::ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
{
    // Предпочитаем BGRA8 UNORM (без автоматической гамма-коррекции)
    // X-Ray Engine D3D11 не использует sRGB конверсию, поэтому для совместимости
    // цветов используем UNORM формат. Иначе текстуры будут выглядеть слишком яркими
    // и с розоватым оттенком из-за двойной гамма-коррекции.
    for (const auto& format : availableFormats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            Msg("[Vulkan] Selected BGRA8 UNORM swapchain format (D3D11 compatible)");
            return format;
        }
    }

    // Fallback: BGRA8 SRGB (если UNORM недоступен)
    for (const auto& format : availableFormats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            Msg("[Vulkan] WARNING: Using SRGB swapchain format, colors may differ from D3D11");
            return format;
        }
    }

    // Fallback: первый доступный
    Msg("[Vulkan] WARNING: Using fallback swapchain format");
    return availableFormats[0];
}

#ifdef __SWITCH__
extern "C" int g_xr_nx_present_mode;   // xr_3da/x_ray.cpp, nx_present.txt (v118)
extern "C" int g_xr_nx_swap_images;    // xr_3da/x_ray.cpp, nx_present.txt "img=N" (v119)
#endif

extern u32 g_dwFPSlimit;   // xr_3da/device.cpp, r_fps_lock (0 = nofpslock)

u32 CVulkanSwapchain::PresentKey()
{
    return (psDeviceFlags.test(rsVSync) ? 1u : 0u) | (g_dwFPSlimit == 0 ? 2u : 0u);
}

// Выбор present mode
VkPresentModeKHR CVulkanSwapchain::ChoosePresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes)
{
    auto has = [&](VkPresentModeKHR m) {
        return std::find(availablePresentModes.begin(), availablePresentModes.end(), m)
               != availablePresentModes.end();
    };

    // Launch-param overrides. FIFO (steady, every-frame vsync) is the most
    // compatible present mode for screen-recording / capture software — MAILBOX
    // lets the driver silently DROP already-queued frames, which OBS & co. see
    // as stutter/instability. Give the user a lever to force it (or to go fully
    // uncapped) without a rebuild.
    const bool forceVsync = Core.Params && strstr(Core.Params, "-vsync");
    const bool noVsync    = Core.Params && strstr(Core.Params, "-no_vsync");
#ifdef __SWITCH__
    {   // v118: one-shot nx_present.txt (x_ray.cpp) -- 1 immediate, 2 fifo_relaxed, 3 mailbox; 0 = the usual choice.
        static const VkPresentModeKHR kWant[4] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                  VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
        static const char* const kName[4] = {"FIFO", "IMMEDIATE", "FIFO_RELAXED", "MAILBOX"};
        const int w = g_xr_nx_present_mode;
        if (w > 0 && w < 4) {
            if (has(kWant[w])) { Msg("[Vulkan] Using %s present mode (nx_present.txt)", kName[w]); return kWant[w]; }
            Msg("![Vulkan] nx_present.txt asked for %s, the surface does not offer it -- usual choice", kName[w]);
        }
    }
#endif

    if (forceVsync) {
        Msg("[Vulkan] Using FIFO present mode (vsync — forced by -vsync, recorder-friendly)");
        return VK_PRESENT_MODE_FIFO_KHR;  // guaranteed available by the spec
    }
    if (noVsync && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        Msg("[Vulkan] Using IMMEDIATE present mode (-no_vsync — uncapped, may tear)");
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // ⭐rs_v_sync — the options-screen checkbox. It was bound to a psDeviceFlags
    // bit that no line of Vulkan code read: the present mode came from the launch
    // params and nothing else, so the box toggled and the frame pacing did not.
    // The launch params stay ABOVE it on purpose — they are a deliberate override
    // for a capture session and should not be argued with by a saved config.
    // Needs `vid_restart`; the swapchain owns the present mode.
    if (psDeviceFlags.test(rsVSync)) {
        Msg("[Vulkan] Using FIFO present mode (vsync — rs_v_sync on)");
        return VK_PRESENT_MODE_FIFO_KHR;  // guaranteed available by the spec
    }

#ifndef __SWITCH__
    // ⭐r_fps_lock "nofpslock" means NO CAP, and MAILBOX is not that. On Windows the
    // NVIDIA driver presents MAILBOX through a DXGI flip chain that releases images at
    // the refresh rate, so "unlimited" sat flat at the monitor's 240 Hz. 📏03-10.
    // IMMEDIATE is the only mode that never waits for a vblank (it may tear outside a
    // G-SYNC/FreeSync range — that is the deal the player picked). With a limit set,
    // MAILBOX stays: the limiter paces the frames and nothing tears.
    if (g_dwFPSlimit == 0 && has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        Msg("[Vulkan] Using IMMEDIATE present mode (r_fps_lock off — uncapped, may tear)");
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
#endif

    // NB (DLSS-G): the FG plugin reports "VSync with FG: not supported" — Frame
    // Generation wants vsync OFF (its pacer owns frame timing). FIFO (= vsync) makes
    // the pacer fight it (SyncInterval 0<->1 toggling -> "Out of order frame - skip
    // the present"), so we do NOT force FIFO here. MAILBOX below is vsync-off-ish
    // (no tearing, no hard vsync) and was the most stable with FG; for the cleanest
    // FG pacing use -no_vsync (IMMEDIATE) above.

    // Default: MAILBOX (triple buffering, low latency) when available.
    if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
        Msg("[Vulkan] Using MAILBOX present mode (triple buffering)");
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }

    // Fallback: FIFO (vsync, always available)
    Msg("[Vulkan] Using FIFO present mode (vsync)");
    return VK_PRESENT_MODE_FIFO_KHR;
}

// Выбор extent
VkExtent2D CVulkanSwapchain::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height)
{
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }

    VkExtent2D actualExtent = { width, height };

    actualExtent.width = std::max(capabilities.minImageExtent.width,
                                   std::min(capabilities.maxImageExtent.width, actualExtent.width));
    actualExtent.height = std::max(capabilities.minImageExtent.height,
                                    std::min(capabilities.maxImageExtent.height, actualExtent.height));

    return actualExtent;
}

// Создание swapchain
void CVulkanSwapchain::Create(u32 width, u32 height)
{
    // Проверка на минимизацию
    if (width == 0 || height == 0) {
        Msg("[Vulkan] Window minimized, skipping swapchain creation");
        m_IsMinimized = true;
        return;
    }
    m_IsMinimized = false;

    // Получаем capabilities
    VkSurfaceCapabilitiesKHR capabilities;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VulkanHW.m_PhysicalDevice, VulkanHW.m_Surface, &capabilities);

    // Получаем форматы
    u32 formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(VulkanHW.m_PhysicalDevice, VulkanHW.m_Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(VulkanHW.m_PhysicalDevice, VulkanHW.m_Surface, &formatCount, formats.data());

    // Получаем present modes
    u32 presentModeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(VulkanHW.m_PhysicalDevice, VulkanHW.m_Surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(VulkanHW.m_PhysicalDevice, VulkanHW.m_Surface, &presentModeCount, presentModes.data());

    {   // v118: what the surface offers (the Switch run picked FIFO by fallback -- MAILBOX absent?)
        string256 ms; ms[0] = 0;
        for (VkPresentModeKHR m : presentModes) {
            const char* n = m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                          : m == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "other";
            xr_strcat(ms, n); xr_strcat(ms, " ");
        }
        Msg("[Vulkan] Surface present modes: %s", ms);
    }

    // Выбираем параметры
    VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
    VkPresentModeKHR presentMode = ChoosePresentMode(presentModes);
    m_PresentKey = PresentKey();
    VkExtent2D extent = ChooseSwapExtent(capabilities, width, height);

    // Количество images (triple buffering)
    u32 imageCount = capabilities.minImageCount + 1;
#ifdef __SWITCH__
    // v119: on Horizon a submission's acquire wait is enqueued ahead of the WHOLE frame, so the scene cannot start
    // until the display gives an image back; more images = the next one is usually free already.
    Msg("[Vulkan] Surface image count: min %u max %u", capabilities.minImageCount, capabilities.maxImageCount);
    if (g_xr_nx_swap_images > 0) {
        imageCount = (u32)g_xr_nx_swap_images < capabilities.minImageCount ? capabilities.minImageCount : (u32)g_xr_nx_swap_images;
        Msg("[Vulkan] Swapchain image count %u requested by nx_present.txt", imageCount);
    }
#endif
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    Msg("[Vulkan] Creating swapchain: %dx%d, %u images", extent.width, extent.height, imageCount);

    // Создаём swapchain
    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = VulkanHW.m_Surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    u32 queueFamilyIndices[] = { VulkanHW.m_GraphicsFamily, VulkanHW.m_PresentFamily };

    if (VulkanHW.m_GraphicsFamily != VulkanHW.m_PresentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = 0;
        createInfo.pQueueFamilyIndices = nullptr;
    }

    createInfo.preTransform = capabilities.currentTransform;
#ifdef __ANDROID__
    // A phone's panel is portrait: in landscape currentTransform is ROTATE_90/270, and passing it back says
    // "the image is already rotated" -- the menu came out turned by 90 degrees (14-10, OnePlus 12). IDENTITY
    // lets the compositor rotate (each present then returns SUBOPTIMAL); real pre-rotation of the final pass
    // would save that composition step.
    Msg("[Vulkan] Surface transform: current 0x%x, supported 0x%x", capabilities.currentTransform, capabilities.supportedTransforms);
    if (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        createInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(VulkanHW.m_Device, &createInfo, nullptr, &m_Swapchain));

    // Получаем images
    vkGetSwapchainImagesKHR(VulkanHW.m_Device, m_Swapchain, &imageCount, nullptr);
    m_Images.resize(imageCount);
    vkGetSwapchainImagesKHR(VulkanHW.m_Device, m_Swapchain, &imageCount, m_Images.data());

    m_Format = surfaceFormat.format;
    m_Extent = extent;
    m_ImageCount = imageCount;
    m_DepthExtent = { 0, 0 };   // re-default depth to the new swapchain extent; Begin's ResizeDepth refines it if DLSS upscales

    // Создаём image views
    m_ImageViews.resize(imageCount);
    for (u32 i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_Format;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(VulkanHW.m_Device, &viewInfo, nullptr, &m_ImageViews[i]));
    }

    // Per-image "render finished" semaphores — one per swapchain image, waited by
    // vkQueuePresentKHR (see m_RenderFinished doc in the header). Created here and
    // destroyed in Destroy() so they track the image set across resizes.
    m_RenderFinished.resize(imageCount);
    for (u32 i = 0; i < imageCount; i++) {
        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VK_CHECK(vkCreateSemaphore(VulkanHW.m_Device, &semInfo, nullptr, &m_RenderFinished[i]));
    }

    // Создаём depth buffer
    CreateDepthResources();

    Msg("[Vulkan] Swapchain created successfully");
}

// Уничтожение swapchain
void CVulkanSwapchain::Destroy()
{
    if (VulkanHW.m_Device == VK_NULL_HANDLE) return;

    // Уничтожаем depth buffer
    DestroyDepthResources();

    // Per-image render-finished semaphores. Recreate() calls vkDeviceWaitIdle
    // first, so no submit/present still references them here.
    for (auto sem : m_RenderFinished) {
        if (sem != VK_NULL_HANDLE) vkDestroySemaphore(VulkanHW.m_Device, sem, nullptr);
    }
    m_RenderFinished.clear();

    // Уничтожаем image views
    for (auto imageView : m_ImageViews) {
        vkDestroyImageView(VulkanHW.m_Device, imageView, nullptr);
    }
    m_ImageViews.clear();

    // Drop the frame graph's layout entry for every image BEFORE the swapchain
    // (and with it the images) goes away. MANDATORY, not tidiness: the driver
    // hands out the same VkImage handle values for the recreated swapchain, so a
    // surviving entry would tell the next frame that a brand-new image is already
    // in PRESENT_SRC — and the transition that should have happened would be
    // skipped. Resize is exactly when that bites.
    for (auto image : m_Images) {
        if (image != VK_NULL_HANDLE) VK::g_FrameGraph.Forget(image);
    }

    // Уничтожаем swapchain
    if (m_Swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(VulkanHW.m_Device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }

    m_Images.clear();

    Msg("[Vulkan] Swapchain destroyed");
}

// Пересоздание swapchain (при resize)
void CVulkanSwapchain::Recreate(u32 width, u32 height)
{
    Msg("[Vulkan] Recreating swapchain: %dx%d", width, height);

    // Ждём завершения работы
    VK_DeviceWaitIdleLocked();

    // Уничтожаем старый swapchain
    Destroy();

    // Создаём новый
    Create(width, height);
}

// Получение следующего image
u32 CVulkanSwapchain::AcquireNextImage(VkSemaphore semaphore, VkFence fence)
{
    // rs_v_sync / r_fps_lock moved since the swapchain was made: rebuild it now, the
    // same way an OUT_OF_DATE acquire does below (that is all vid_restart does here too).
    if (m_Swapchain != VK_NULL_HANDLE && PresentKey() != m_PresentKey) {
        Msg("[Vulkan] Present settings changed (vsync %d, fps lock %s) - recreating swapchain",
            psDeviceFlags.test(rsVSync) ? 1 : 0, g_dwFPSlimit ? "on" : "off");
        Recreate(m_Extent.width, m_Extent.height);
    }

    u32 imageIndex = 0;
    VkResult result;
    {
        LoadProf::Scope _lp("rt.acquire");
        result = vkAcquireNextImageKHR(VulkanHW.m_Device, m_Swapchain, UINT64_MAX, semaphore, fence, &imageIndex);
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        Msg("[Vulkan] Swapchain out of date during acquire - recreating");
        // Recreate swapchain
        VK_DeviceWaitIdleLocked();
        Recreate(m_Extent.width, m_Extent.height);
        // Try acquire again
        result = vkAcquireNextImageKHR(VulkanHW.m_Device, m_Swapchain, UINT64_MAX,
                                        semaphore, fence, &imageIndex);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            Msg("![Vulkan] Failed to acquire after swapchain recreate: %d", result);
            return UINT32_MAX;
        }
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        Msg("![Vulkan] Failed to acquire swapchain image: %d", result);
        return UINT32_MAX;
    }

    // Update current image tracking
    m_CurrentImageIndex = imageIndex;
    m_CurrentImage = m_Images[imageIndex];
    m_CurrentImageView = m_ImageViews[imageIndex];

    return imageIndex;
}

// Present
void CVulkanSwapchain::Present(VkSemaphore waitSemaphore, u32 imageIndex)
{
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Swapchain;
    presentInfo.pImageIndices = &imageIndex;
    presentInfo.pResults = nullptr;

    VkResult result;
    {
        LoadProf::Scope _lp("rt.present"); // with the queue lock's wait
        std::lock_guard<std::mutex> queueLock(g_VkQueueMx);   // RENDER_THREAD_PLAN step 10: one lock for every queue submit/present/wait
        result = vkQueuePresentKHR(VulkanHW.m_PresentQueue, &presentInfo);
    }

    // SUBOPTIMAL can come back on every present (Android with an IDENTITY preTransform): say it once.
    static bool s_said_suboptimal = false;
    if (result == VK_ERROR_OUT_OF_DATE_KHR || (result == VK_SUBOPTIMAL_KHR && !s_said_suboptimal)) {
        s_said_suboptimal |= result == VK_SUBOPTIMAL_KHR;
        Msg("[Vulkan] Swapchain out of date/suboptimal during present (result=%d)", result);
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        Msg("![Vulkan] Failed to present swapchain image: %d", result);
        if (result == VK_ERROR_DEVICE_LOST) VK::Prof::DumpCheckpoints("Present");
    }
}

// ============================================================================
// Depth Buffer
// ============================================================================

// Поиск поддерживаемого формата
VkFormat CVulkanSwapchain::FindSupportedFormat(const std::vector<VkFormat>& candidates,
                                                VkImageTiling tiling,
                                                VkFormatFeatureFlags features)
{
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(VulkanHW.m_PhysicalDevice, format, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
            return format;
        } else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    Msg("!Failed to find supported format");
    return VK_FORMAT_UNDEFINED;
}

// Поиск depth формата
VkFormat CVulkanSwapchain::FindDepthFormat()
{
    // Prefer the depth-only D32_SFLOAT. The forward Vulkan renderer never uses
    // stencil (every pipeline leaves stencilAttachmentFormat = UNDEFINED and
    // stencilTestEnable = FALSE). Picking a depth+stencil format would force the
    // depth barrier/view to carry a stencil aspect while the attachment layout is
    // DEPTH_ATTACHMENT_OPTIMAL (depth-only) — that mismatch trips
    // VUID-VkImageMemoryBarrier2-aspectMask-08703. Depth+stencil formats stay as
    // fallbacks (D32_SFLOAT is a mandatory format, so they should never be reached).
    // When the deferred path is un-parked and needs stencil, revisit this.
    std::vector<VkFormat> candidates = {
        VK_FORMAT_D32_SFLOAT,           // 32-bit float depth only (preferred, no stencil)
        VK_FORMAT_D32_SFLOAT_S8_UINT,   // 32-bit float depth + 8-bit stencil (fallback)
        VK_FORMAT_D24_UNORM_S8_UINT,    // 24-bit depth + 8-bit stencil (fallback)
    };

    VkFormat format = FindSupportedFormat(
        candidates,
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
    );

    if (format != VK_FORMAT_UNDEFINED) {
        Msg("[Vulkan] Selected depth format: %d", format);
        return format;
    }

    Msg("!Failed to find depth format");
    return VK_FORMAT_D32_SFLOAT;  // Fallback
}

// Создание depth resources
void CVulkanSwapchain::CreateDepthResources()
{
    // Находим подходящий depth format
    m_DepthFormat = FindDepthFormat();

    // Depth defaults to the swapchain extent; ResizeDepth() can shrink it to the DLSS
    // render resolution (render<display upscale). Track the actual depth size separately.
    if (m_DepthExtent.width == 0 || m_DepthExtent.height == 0) m_DepthExtent = m_Extent;

    // Создаём depth image через VMA
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = m_DepthExtent.width;
    imageInfo.extent.height = m_DepthExtent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = m_DepthFormat;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // SAMPLED_BIT lets the grass HZB-build compute pass sample this depth buffer
    // (max-reduce into the Hi-Z pyramid). Harmless for attachment usage.
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    allocInfo.priority = 1.0f;   // main depth buffer — hot, never evict before streamable textures

    VK_CHECK(VK::Vram::CreateImage(VulkanHW.m_Allocator, &imageInfo, &allocInfo,
                            &m_DepthImage, &m_DepthAllocation, nullptr));

    // Создаём image view для depth
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_DepthImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = m_DepthFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (m_DepthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT || m_DepthFormat == VK_FORMAT_D24_UNORM_S8_UINT)
        viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VK_CHECK(vkCreateImageView(VulkanHW.m_Device, &viewInfo, nullptr, &m_DepthView));
    m_DepthImageView = m_DepthView;  // Sync alias

    Msg("[Vulkan] Depth buffer created: %dx%d, format %d",
        m_DepthExtent.width, m_DepthExtent.height, m_DepthFormat);
}

// Recreate the scene depth at a specific extent (DLSS render<display). No-op when the
// depth already has that size. The caller (CRender::Begin) guarantees the GPU is idle
// w.r.t. the old depth before calling (it only fires on a render-resolution change).
void CVulkanSwapchain::ResizeDepth(VkExtent2D extent)
{
    if (extent.width == 0 || extent.height == 0) return;
    if (m_DepthImage != VK_NULL_HANDLE && extent.width == m_DepthExtent.width && extent.height == m_DepthExtent.height)
        return;   // already the right size
    DestroyDepthResources();
    m_DepthExtent = extent;
    CreateDepthResources();
}

// Уничтожение depth resources
void CVulkanSwapchain::DestroyDepthResources()
{
    if (m_DepthView != VK_NULL_HANDLE) {
        vkDestroyImageView(VulkanHW.m_Device, m_DepthView, nullptr);
        m_DepthView = VK_NULL_HANDLE;
        m_DepthImageView = VK_NULL_HANDLE;  // Clear alias
    }

    if (m_DepthImage != VK_NULL_HANDLE) {
        VK::Vram::DestroyImage(VulkanHW.m_Allocator, m_DepthImage, m_DepthAllocation);
        m_DepthImage = VK_NULL_HANDLE;
        m_DepthAllocation = VK_NULL_HANDLE;
    }
}
