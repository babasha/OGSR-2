// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "HW_Vulkan.h"
#include "nvsdk_ngx_vk.h"  // NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements - per-FEATURE device extensions
#include "vk_sl.h"          // VK::SL — Streamline init/probe (must precede vkCreateInstance)
#include "vk_geometry.h"
#include "vk_lighting.h"
#include "vk_authorship.h"
#include "vk_command_buffer.h"   // CommandManager — single-time helpers delegate to the immediate path
#include "vk_vram_stats.h"       // VK::Vram::DestroySmallPools — before vmaDestroyAllocator
#include <vector>
#include <set>
#ifdef __ANDROID__
extern "C" void xr_vk_android_load_device(VkDevice device); // vk_android_entry.cpp: 1.2/1.3 entry points the NDK stub lacks
#endif
#ifndef _WIN32
#  include <SDL2/SDL.h>
#endif

// Streamline is only needed by the SL/FG routes — Step 0 below reads these to decide
// whether to bring the interposer up at all (vk_console_min.cpp owns them).
extern int ps_r_dlss_sl;
extern int ps_r_dlss_fg;
extern u32 ps_r_dlss_nr_featid;   // r_dlss_nr_featid — Neural Rendering's NGX feature ID (no public constant)

// Use VK namespace globals
using VK::g_VulkanGeometry;
using VK::g_VulkanLighting;

// Глобальный экземпляр Vulkan HW (переименован чтобы избежать конфликта с R4)
CVulkanHW VulkanHW;
std::mutex g_VkQueueMx;   // see HW_Vulkan.h

CVulkanHW::CVulkanHW()
{
    // DON'T use Msg() in global constructors - logging system isn't initialized yet!
    // Msg("[Vulkan] CVulkanHW constructor");
}

CVulkanHW::~CVulkanHW()
{
    // Msg("[Vulkan] CVulkanHW destructor");
}

// Поиск queue families (graphics + present)
bool CVulkanHW::FindQueueFamilies()
{
    u32 queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());

    // Ищем graphics queue и present queue
    m_GraphicsFamily = UINT32_MAX;
    m_PresentFamily = UINT32_MAX;

    for (u32 i = 0; i < queueFamilyCount; i++) {
        // Graphics queue
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_GraphicsFamily = i;
        }

        // Present queue
        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(m_PhysicalDevice, i, m_Surface, &presentSupport);
        if (presentSupport) {
            m_PresentFamily = i;
        }

        // Если нашли обе - выходим
        if (m_GraphicsFamily != UINT32_MAX && m_PresentFamily != UINT32_MAX) {
            break;
        }
    }

    if (m_GraphicsFamily == UINT32_MAX) {
        Msg("!Failed to find graphics queue family");
        return false;
    }

    if (m_PresentFamily == UINT32_MAX) {
        Msg("!Failed to find present queue family");
        return false;
    }

    Msg("[Vulkan] Graphics queue family: %u", m_GraphicsFamily);
    if (m_GraphicsFamily == m_PresentFamily) {
        Msg("[Vulkan] Present queue family: %u (same as graphics)", m_PresentFamily);
    } else {
        Msg("[Vulkan] Present queue family: %u", m_PresentFamily);
    }

    // --- Dedicated compute family (Layer 1: discovery only, not yet submitted to) ---
    // Prefer a COMPUTE family WITHOUT graphics → a genuinely async-capable queue.
    // Fall back to any compute-capable family (usually == graphics) so the handle
    // is always valid; callers gate real overlap on (m_ComputeFamily != m_GraphicsFamily).
    m_ComputeFamily = UINT32_MAX;
    for (u32 i = 0; i < queueFamilyCount; i++) {
        const VkQueueFlags f = queueFamilies[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) { m_ComputeFamily = i; break; }
    }
    if (m_ComputeFamily == UINT32_MAX) {
        for (u32 i = 0; i < queueFamilyCount; i++)
            if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { m_ComputeFamily = i; break; }
    }
    if (m_ComputeFamily == UINT32_MAX) m_ComputeFamily = m_GraphicsFamily;  // last resort

    // --- Dedicated transfer family ---
    // Prefer a pure DMA family (TRANSFER without graphics/compute) for off-timeline
    // uploads. Note: per spec GRAPHICS/COMPUTE families support transfer implicitly,
    // so the fallback to the graphics family is always valid.
    m_TransferFamily = UINT32_MAX;
    for (u32 i = 0; i < queueFamilyCount; i++) {
        const VkQueueFlags f = queueFamilies[i].queueFlags;
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && !(f & VK_QUEUE_COMPUTE_BIT)) { m_TransferFamily = i; break; }
    }
    if (m_TransferFamily == UINT32_MAX) m_TransferFamily = m_GraphicsFamily;  // graphics implies transfer

    Msg("[Vulkan] Compute queue family: %u%s", m_ComputeFamily,
        (m_ComputeFamily == m_GraphicsFamily) ? " (shared with graphics — no async compute)" : " (dedicated, async-capable)");
    Msg("[Vulkan] Transfer queue family: %u%s", m_TransferFamily,
        (m_TransferFamily == m_GraphicsFamily) ? " (shared with graphics)" : " (dedicated DMA)");

    return true;
}

// Создание logical device
bool CVulkanHW::CreateLogicalDevice()
{
    // Queue create infos
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    // std::set dedups, so families that coincide (e.g. compute == graphics on a
    // single-family GPU) request just one queue; their handles will then alias below.
    std::set<u32> uniqueQueueFamilies = { m_GraphicsFamily, m_PresentFamily, m_ComputeFamily, m_TransferFamily };

    float queuePriority = 1.0f;
    for (u32 queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo = {};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    // Vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features13 = {};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    features13.maintenance4 = VK_TRUE;
    // Our fragment shaders compile `discard` to OpDemoteToHelperInvocation (glslc does
    // this by default for SPIR-V 1.6), so the modules DECLARE the capability while the
    // device never enabled it — 68 × VUID-VkShaderModuleCreateInfo-pCode-08740 per run,
    // one per module. Core 1.3 and universal on anything that reaches this renderer.
    features13.shaderDemoteToHelperInvocation = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.separateDepthStencilLayouts = VK_TRUE;
    features12.drawIndirectCount = VK_TRUE;
    features12.timelineSemaphore = VK_TRUE;
    features12.pNext = &features13;

    // Query descriptor indexing support for bindless textures
    {
        VkPhysicalDeviceVulkan12Features supported12 = {};
        supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceFeatures2 supported = {};
        supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        supported.pNext = &supported12;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &supported);

        m_bBindlessSupported =
            supported12.descriptorBindingPartiallyBound &&
            supported12.runtimeDescriptorArray &&
            supported12.shaderSampledImageArrayNonUniformIndexing;

        if (m_bBindlessSupported)
        {
            features12.descriptorBindingPartiallyBound = VK_TRUE;
            features12.runtimeDescriptorArray = VK_TRUE;
            features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            Msg("[Vulkan] Bindless descriptor indexing: ENABLED");
        }
        else
        {
            Msg("[Vulkan] Bindless descriptor indexing: NOT SUPPORTED (fallback to per-type loop)");
        }
    }

    // ⭐⭐FidelityFX (FSR 3.1) NEEDS THESE THREE, and the way it needs them is a
    // trap: its Vulkan backend probes vkEnumerateDeviceExtensionProperties, which
    // reports what the driver SUPPORTS, not what we ENABLED. Seeing
    // VK_EXT_subgroup_size_control it then builds pipelines with
    // VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT, and seeing
    // VK_KHR_shader_float16_int8 it picks the FP16 shader permutations - both of
    // which are illegal unless the FEATURE is on. All three are core in 1.2/1.3,
    // so there is no extension to add here, only a feature bit; and the failure
    // mode without them is a wall of VUIDs plus a black upscale, not a clean error.
    // ⚠Queried, not assumed: on a device that lacks them FSR falls back to its
    // scalar wave32 path, while asking for an unsupported feature fails device
    // creation outright - which would take the whole renderer down with it.
    {
        VkPhysicalDeviceVulkan13Features sup13 = {};
        sup13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features sup12 = {};
        sup12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        sup12.pNext = &sup13;
        VkPhysicalDeviceFeatures2 sup = {};
        sup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        sup.pNext = &sup12;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &sup);

        if (sup13.subgroupSizeControl)  features13.subgroupSizeControl  = VK_TRUE;
        if (sup13.computeFullSubgroups) features13.computeFullSubgroups = VK_TRUE;
        if (sup12.shaderFloat16)        features12.shaderFloat16        = VK_TRUE;
        Msg("[Vulkan] FSR prerequisites: subgroupSizeControl=%d computeFullSubgroups=%d shaderFloat16=%d",
            (int)sup13.subgroupSizeControl, (int)sup13.computeFullSubgroups, (int)sup12.shaderFloat16);
    }
    // Device features
    VkPhysicalDeviceFeatures2 deviceFeatures = {};
    deviceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    deviceFeatures.features.samplerAnisotropy = VK_TRUE;
    deviceFeatures.features.fillModeNonSolid = VK_TRUE;
    deviceFeatures.features.wideLines = VK_TRUE;
    deviceFeatures.features.multiDrawIndirect = VK_TRUE;
    deviceFeatures.features.drawIndirectFirstInstance = VK_TRUE;  // trees: firstInstance encodes global tree index
    deviceFeatures.features.shaderClipDistance = VK_TRUE;         // VSM: gl_ClipDistance clips page geometry to its atlas sub-rect
    deviceFeatures.features.imageCubeArray = VK_TRUE;             // point shadow POOL: array of shadow cubes (samplerCubeArray)
    deviceFeatures.features.fragmentStoresAndAtomics = VK_TRUE;   // texture-streaming GPU feedback: world FS atomicMin desired mips
    deviceFeatures.features.pipelineStatisticsQuery = VK_TRUE;    // VRS diag: FS-invocation counter around the world color pass
    deviceFeatures.pNext = &features12;

    // Unconditional features are only enabled if the driver has them. Asking for a missing one fails
    // vkCreateDevice outright (VK_ERROR_FEATURE_NOT_PRESENT), which takes the whole renderer down
    // before it can say WHICH features are the problem. NVK on the Switch's GM20B lacks some of them
    // (drawIndirectCount was the first to show up); everything dropped here is listed in the log so
    // the gaps can be fixed in one pass. On a device that has them all this changes nothing.
    {
        VkPhysicalDeviceVulkan13Features s13 = {};
        s13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features s12 = {};
        s12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        s12.pNext = &s13;
        VkPhysicalDeviceFeatures2 s2 = {};
        s2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        s2.pNext = &s12;
        vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &s2);
#define XR_SOFT_FEATURE(dst, src, name) \
        if ((dst) && !(src)) { (dst) = VK_FALSE; Msg("![Vulkan] MISSING feature %s -- NOT enabled, dependent paths will misbehave", name); }
        XR_SOFT_FEATURE(features12.bufferDeviceAddress, s12.bufferDeviceAddress, "bufferDeviceAddress")
        XR_SOFT_FEATURE(features12.separateDepthStencilLayouts, s12.separateDepthStencilLayouts, "separateDepthStencilLayouts")
        XR_SOFT_FEATURE(features12.drawIndirectCount, s12.drawIndirectCount, "drawIndirectCount")
        XR_SOFT_FEATURE(features12.timelineSemaphore, s12.timelineSemaphore, "timelineSemaphore")
        XR_SOFT_FEATURE(features13.dynamicRendering, s13.dynamicRendering, "dynamicRendering")
        XR_SOFT_FEATURE(features13.synchronization2, s13.synchronization2, "synchronization2")
        XR_SOFT_FEATURE(features13.maintenance4, s13.maintenance4, "maintenance4")
        XR_SOFT_FEATURE(features13.shaderDemoteToHelperInvocation, s13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation")
        const VkPhysicalDeviceFeatures& b = s2.features;
        XR_SOFT_FEATURE(deviceFeatures.features.samplerAnisotropy, b.samplerAnisotropy, "samplerAnisotropy")
        XR_SOFT_FEATURE(deviceFeatures.features.fillModeNonSolid, b.fillModeNonSolid, "fillModeNonSolid")
        XR_SOFT_FEATURE(deviceFeatures.features.wideLines, b.wideLines, "wideLines")
        XR_SOFT_FEATURE(deviceFeatures.features.multiDrawIndirect, b.multiDrawIndirect, "multiDrawIndirect")
        XR_SOFT_FEATURE(deviceFeatures.features.drawIndirectFirstInstance, b.drawIndirectFirstInstance, "drawIndirectFirstInstance")
        XR_SOFT_FEATURE(deviceFeatures.features.shaderClipDistance, b.shaderClipDistance, "shaderClipDistance")
        XR_SOFT_FEATURE(deviceFeatures.features.imageCubeArray, b.imageCubeArray, "imageCubeArray")
        XR_SOFT_FEATURE(deviceFeatures.features.fragmentStoresAndAtomics, b.fragmentStoresAndAtomics, "fragmentStoresAndAtomics")
        XR_SOFT_FEATURE(deviceFeatures.features.pipelineStatisticsQuery, b.pipelineStatisticsQuery, "pipelineStatisticsQuery")
#undef XR_SOFT_FEATURE
    }

    // Tessellation (world heightmap displacement, R4 TESS_HM). Universal on
    // desktop GPUs, but query anyway — when absent the renderer keeps the
    // plain triangle pipelines and PipelineCache::TessAvailable() stays false.
    {
        VkPhysicalDeviceFeatures supportedBase{};
        vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supportedBase);
        m_bTessellationSupported = supportedBase.tessellationShader == VK_TRUE;
        if (m_bTessellationSupported)
            deviceFeatures.features.tessellationShader = VK_TRUE;
        else
            Msg("[Vulkan] tessellationShader not supported — world tessellation disabled");

        // independentBlend: per-attachment blend state on an MRT pipeline. The water
        // mask pass needs it — its two targets share the MAX blend but differ in
        // colorWriteMask (R only vs RGBA, vk_pass_water.cpp), and WITHOUT this feature
        // the spec requires every pAttachments element to be identical. The pipeline
        // was being created anyway (VUID-...-pAttachments-00605, caught 16-08), i.e.
        // the write masks were only working by driver grace.
        if (supportedBase.independentBlend == VK_TRUE)
            deviceFeatures.features.independentBlend = VK_TRUE;
        else
            Msg("![Vulkan] independentBlend NOT supported — MRT passes with per-attachment "
                "write masks (water mask) are out of spec on this GPU");

        // occlusionQueryPrecise — REQUIRED BY DIAGNOSTICS THAT WERE ALREADY SHIPPING
        // WITHOUT IT. Two places pass VK_QUERY_CONTROL_PRECISE_BIT to vkCmdBeginQuery:
        // the r_fsinv_split occlusion query (vk_vrs.cpp) and the tree ledger's
        // samples-kept counter. That bit is ILLEGAL unless this feature is on, and
        // without it a driver may legally answer with any non-zero value rather than
        // a true sample count — so the number was not merely unvalidated, it was not
        // required to be correct. Caught by -vk_validation on 25-08
        // (VUID-vkCmdBeginQuery-queryType-00800).
        // ⚠ Probed, not asserted: enabling an unsupported feature fails vkCreateDevice
        // outright, and a diagnostic must never be able to cost someone the renderer.
        m_bOcclusionQueryPrecise = supportedBase.occlusionQueryPrecise == VK_TRUE;
        if (m_bOcclusionQueryPrecise)
            deviceFeatures.features.occlusionQueryPrecise = VK_TRUE;
        else
            Msg("[Vulkan] occlusionQueryPrecise not supported — samples-passed counters "
                "fall back to imprecise (non-zero) results");
    }

    // Build final extension list: required + optional NGX extensions if available
    std::vector<const char*> enabledExtensions(g_DeviceExtensions, g_DeviceExtensions + g_DeviceExtensionCount);

    // Check which optional NGX extensions are supported
    {
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availableExts(extCount);
        vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, availableExts.data());

        u32 ngxFound = 0;
        for (u32 i = 0; i < g_NgxDeviceExtensionCount; i++) {
            bool found = false;
            for (const auto& ext : availableExts) {
                if (strcmp(g_NgxDeviceExtensions[i], ext.extensionName) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) {
                enabledExtensions.push_back(g_NgxDeviceExtensions[i]);
                ngxFound++;
                Msg("[Vulkan] NGX extension enabled: %s", g_NgxDeviceExtensions[i]);
            } else {
                Msg("[Vulkan] NGX extension not available: %s", g_NgxDeviceExtensions[i]);
            }
        }
        g_bNgxExtensionsEnabled = (ngxFound == g_NgxDeviceExtensionCount);
        Msg("[Vulkan] NGX extensions: %u/%u available (hardcoded list)", ngxFound, g_NgxDeviceExtensionCount);

        // ⭐ASK NGX WHAT IT ACTUALLY NEEDS, PER FEATURE. The three names above are a
        // hardcoded list from when only Super Resolution existed. Ray Reconstruction is
        // a DIFFERENT network and may require a different (larger) set — and NGX is
        // explicit that this query "should be called before the VkDevice for the
        // application is created so that the correct extensions can be enabled", which
        // is exactly here.
        // ⚠A missing extension does not fail loudly: NGX falls back to a slower path,
        // and that is indistinguishable from "this feature is simply expensive" unless
        // somebody prints the difference. So print it, and enable what the device has.
#if !defined(__aarch64__) // no NGX SDK on arm64 (see the Dlss stub in vk_dlss.cpp)
        {
            NVSDK_NGX_FeatureCommonInfo ngxCommon{};
            struct NgxFeatQuery { const char* name; NVSDK_NGX_Feature id; };
            const NgxFeatQuery feats[] = {
                { "SuperResolution",   NVSDK_NGX_Feature_SuperSampling      },
                { "RayReconstruction", NVSDK_NGX_Feature_RayReconstruction  },
                // ⭐Neural Rendering, whose ID is a CVAR because no public header
                // names it. Asking here is not optional bookkeeping: it is a THIRD
                // network with its own requirements, and the SDK is explicit that
                // the query has to happen before the VkDevice exists — after that,
                // an extension it wants can no longer be enabled. It also doubles as
                // the cheapest probe of whether r_dlss_nr_featid is a real feature:
                // a wrong ID answers nothing, a right one answers with a list.
                { "NeuralRendering",   (NVSDK_NGX_Feature)ps_r_dlss_nr_featid },
            };
            for (const NgxFeatQuery& f : feats) {
                NVSDK_NGX_FeatureDiscoveryInfo di{};
                di.SDKVersion = NVSDK_NGX_Version_API;
                di.FeatureID  = f.id;
                di.Identifier.IdentifierType              = NVSDK_NGX_Application_Identifier_Type_Project_Id;
                di.Identifier.v.ProjectDesc.ProjectId     = "b7f4e2a1-9c3d-4f6b-8a12-0e5d7c9a3b21";
                di.Identifier.v.ProjectDesc.EngineType    = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
                di.Identifier.v.ProjectDesc.EngineVersion = "1.0";
                di.ApplicationDataPath = L".";
                di.FeatureInfo         = &ngxCommon;

                uint32_t               reqCount = 0;
                VkExtensionProperties* reqExts  = nullptr;
                const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                    m_Instance, m_PhysicalDevice, &di, &reqCount, &reqExts);
                if (NVSDK_NGX_FAILED(r) || reqExts == nullptr) {
                    Msg("~[Vulkan] NGX %s: extension query unavailable (0x%08X) - keeping the hardcoded list",
                        f.name, (unsigned)r);
                    continue;
                }
                u32 added = 0, missing = 0;
                for (u32 i = 0; i < reqCount; ++i) {
                    const char* need = reqExts[i].extensionName;
                    bool already = false;
                    for (const char* e : enabledExtensions)
                        if (strcmp(e, need) == 0) { already = true; break; }
                    if (already) continue;
                    bool avail = false;
                    for (const VkExtensionProperties& ext : availableExts)
                        if (strcmp(need, ext.extensionName) == 0) { avail = true; break; }
                    if (avail) {
                        // The name pointers are NGX-owned storage that outlives
                        // vkCreateDevice, so pushing them straight in is safe.
                        enabledExtensions.push_back(need);
                        ++added;
                        Msg("[Vulkan] NGX %s requires %s - ENABLED (absent from the hardcoded list)", f.name, need);
                    } else {
                        ++missing;
                        Msg("![Vulkan] NGX %s requires %s - NOT SUPPORTED by this device", f.name, need);
                    }
                }
                Msg("[Vulkan] NGX %s: %u required device extensions, %u newly enabled, %u unavailable",
                    f.name, reqCount, added, missing);
            }
        }
#endif // !__aarch64__
        Msg("[Vulkan] NGX extensions: %u/%u available", ngxFound, g_NgxDeviceExtensionCount);

        // Check optional RT extensions (ray query + acceleration structure)
        u32 rtFound = 0;
        for (u32 i = 0; i < g_RtDeviceExtensionCount; i++) {
            bool found = false;
            for (const auto& ext : availableExts) {
                if (strcmp(g_RtDeviceExtensions[i], ext.extensionName) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) {
                rtFound++;
                Msg("[Vulkan] RT extension available: %s", g_RtDeviceExtensions[i]);
            } else {
                Msg("[Vulkan] RT extension not available: %s", g_RtDeviceExtensions[i]);
            }
        }

        if (rtFound == g_RtDeviceExtensionCount) {
            // All RT extensions present — query feature support
            VkPhysicalDeviceRayQueryFeaturesKHR rqFeatures = {};
            rqFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures = {};
            asFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            asFeatures.pNext = &rqFeatures;
            VkPhysicalDeviceFeatures2 rtQuery = {};
            rtQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            rtQuery.pNext = &asFeatures;
            vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &rtQuery);

            if (rqFeatures.rayQuery && asFeatures.accelerationStructure) {
                for (u32 i = 0; i < g_RtDeviceExtensionCount; i++)
                    enabledExtensions.push_back(g_RtDeviceExtensions[i]);
                m_bRayQuerySupported = true;
                Msg("[Vulkan] Ray Query + Acceleration Structure: ENABLED");
            } else {
                Msg("[Vulkan] RT extensions present but features not supported (rayQuery=%d, accelStruct=%d)",
                    rqFeatures.rayQuery, asFeatures.accelerationStructure);
            }
        } else {
            Msg("[Vulkan] RT extensions: %u/%u available (need all 3 for RTGI)", rtFound, g_RtDeviceExtensionCount);
        }

        // ⚠⚠⚠VK_KHR_get_memory_requirements2 IS NOT OPTIONAL FOR FSR, and nothing
        // says so until the process dies with EXECUTE at 0x0 and an empty stack trace.
        // The FidelityFX VK backend builds its function table BY NAME and asks for the
        // KHR ALIAS: vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2KHR").
        // That name resolves only while the extension is explicitly enabled - promotion
        // to core 1.1 gives us the UN-SUFFIXED entry point, not the alias - so on our 1.3
        // device it came back NULL. It is then called with no null check at all
        // (ffx_vk.cpp:1663, inside the backend context create; the neighbouring
        // vkSetDebugUtilsObjectNameEXT at :951 IS guarded, which is exactly what makes
        // this one easy to miss), and the jump to address zero takes the client with it.
        // ⭐Symptom to recognise: ffxFsr3UpscalerContextCreate logs NEITHER its success
        // line nor its failure line - the process is simply gone between the two.
        for (const auto& ext : availableExts) {
            if (strcmp(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, ext.extensionName) == 0) {
                enabledExtensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
                Msg("[Vulkan] VK_KHR_get_memory_requirements2: ENABLED (the FSR backend resolves the KHR alias by name)");
                break;
            }
        }

        // Optional: VK_NV_device_diagnostic_checkpoints — GPU progress markers that
        // survive a device loss. Near-zero cost; enables post-mortem "which pass
        // hung" logging on DEVICE_LOST (the level-transition GPU-hang hunt).
        for (const auto& ext : availableExts) {
            if (strcmp(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, ext.extensionName) == 0) {
                enabledExtensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
                m_bCheckpointsSupported = true;
                Msg("[Vulkan] NV diagnostic checkpoints: ENABLED");
                break;
            }
        }
        if (!m_bCheckpointsSupported)
            Msg("[Vulkan] NV diagnostic checkpoints not available");

        // Optional: VK_KHR_fragment_shading_rate (attachment-based VRS) — coarse-shade
        // distant/peripheral pixels via a shading-rate image to cut forward fragment cost.
        {
            bool fsrAvail = false;
            for (const auto& ext : availableExts)
                if (strcmp(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, ext.extensionName) == 0) { fsrAvail = true; break; }
            if (fsrAvail) {
                VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsrFeat = {};
                fsrFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
                VkPhysicalDeviceFeatures2 fsrQuery = {};
                fsrQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                fsrQuery.pNext = &fsrFeat;
                vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &fsrQuery);
                if (fsrFeat.attachmentFragmentShadingRate) {
                    enabledExtensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
                    m_bVRSSupported = true;
                    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrProps = {};
                    fsrProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
                    VkPhysicalDeviceProperties2 props2 = {};
                    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                    props2.pNext = &fsrProps;
                    vkGetPhysicalDeviceProperties2(m_PhysicalDevice, &props2);
                    // Use the coarsest allowed tile (smallest SRI). NVIDIA = 16x16.
                    m_VRSTexelSize = fsrProps.maxFragmentShadingRateAttachmentTexelSize;
                    // Per-pipeline coarse rate (separate feature) — lets us 2x2-shade the
                    // low-freq light cones without an SRI image.
                    m_bVRSPipelineSupported = fsrFeat.pipelineFragmentShadingRate != VK_FALSE;
                    Msg("[Vulkan] VRS (attachment fragment shading rate): ENABLED (tile %ux%u, pipeline-rate %s)",
                        m_VRSTexelSize.width, m_VRSTexelSize.height,
                        m_bVRSPipelineSupported ? "yes" : "no");
                } else {
                    Msg("[Vulkan] VRS extension present but attachmentFragmentShadingRate unsupported");
                }
            } else {
                Msg("[Vulkan] VRS extension not available");
            }
        }

        // Optional: VK_EXT_memory_budget — lets VMA read the driver's LIVE VRAM
        // budget/usage (what the OS + other processes actually left free) instead of
        // guessing from static heap sizes. The texture streamer sizes its residency
        // budget from this; without it we fall back to the heap size. No feature
        // struct — enabling the extension is enough.
        for (const auto& ext : availableExts) {
            if (strcmp(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, ext.extensionName) == 0) {
                enabledExtensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
                m_bMemoryBudgetSupported = true;
                Msg("[Vulkan] VK_EXT_memory_budget: ENABLED (live VRAM budget)");
                break;
            }
        }
        if (!m_bMemoryBudgetSupported)
            Msg("[Vulkan] VK_EXT_memory_budget not available (static heap-size budget)");

        // Optional: VK_EXT_memory_priority — per-allocation eviction priority. We tag
        // streamable world textures LOW so the driver demotes them to system RAM under
        // VRAM pressure before evicting render targets / VSM / geometry (HIGH). Turns a
        // hard OOM into a graceful slowdown. Needs the feature bit chained below.
        {
            bool prioAvail = false;
            for (const auto& ext : availableExts)
                if (strcmp(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, ext.extensionName) == 0) { prioAvail = true; break; }
            if (prioAvail) {
                VkPhysicalDeviceMemoryPriorityFeaturesEXT prioFeat = {};
                prioFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
                VkPhysicalDeviceFeatures2 prioQuery = {};
                prioQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                prioQuery.pNext = &prioFeat;
                vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &prioQuery);
                if (prioFeat.memoryPriority) {
                    enabledExtensions.push_back(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
                    m_bMemoryPrioritySupported = true;
                    Msg("[Vulkan] VK_EXT_memory_priority: ENABLED (fail-soft texture eviction)");
                } else {
                    Msg("[Vulkan] VK_EXT_memory_priority present but feature unsupported");
                }
            } else {
                Msg("[Vulkan] VK_EXT_memory_priority not available");
            }
        }

        // Optional: VK_KHR_pipeline_executable_properties — asks the DRIVER what it
        // compiled a pipeline into: register counts, occupancy, spill, instruction
        // count, per shader stage. Built 26-08 because the crown ubershader kept
        // producing deltas that no amount of removed WORK explains — 5 ALU ops and
        // one varying priced at 0.45 ms — which is the signature of an occupancy
        // threshold rather than of the work itself. Until this, that was a guess; a
        // guess about registers is exactly the kind this project keeps losing.
        //
        // Enabling the extension is free: only pipelines CREATED with
        // VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR pay anything, and that bit is
        // set per pipeline behind r_pipe_stats (see vk_gfx_pipeline.cpp), which must
        // therefore be on BEFORE the level loads.
        {
            bool peAvail = false;
            for (const auto& ext : availableExts)
                if (strcmp(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, ext.extensionName) == 0) { peAvail = true; break; }
            if (peAvail) {
                VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR peFeat = {};
                peFeat.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR;
                VkPhysicalDeviceFeatures2 peQuery = {};
                peQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                peQuery.pNext = &peFeat;
                vkGetPhysicalDeviceFeatures2(m_PhysicalDevice, &peQuery);
                if (peFeat.pipelineExecutableInfo) {
                    enabledExtensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
                    m_bPipeStatsSupported = true;
                    Msg("[Vulkan] VK_KHR_pipeline_executable_properties: ENABLED (r_pipe_stats 1 + vk_pipestats)");
                } else {
                    Msg("[Vulkan] VK_KHR_pipeline_executable_properties present but feature unsupported");
                }
            } else {
                Msg("[Vulkan] VK_KHR_pipeline_executable_properties not available (no register/occupancy readout)");
            }
        }
    }

    // Chain RT feature structs if supported
    VkPhysicalDeviceRayQueryFeaturesKHR enableRayQuery = {};
    enableRayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    enableRayQuery.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR enableAccelStruct = {};
    enableAccelStruct.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    enableAccelStruct.accelerationStructure = VK_TRUE;
    if (m_bRayQuerySupported) {
        enableAccelStruct.pNext = &enableRayQuery;
        features13.pNext = &enableAccelStruct;
    }

    // Chain VRS feature (prepend, preserving whatever is already on features13 — RT or null).
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR enableFsr = {};
    enableFsr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
    enableFsr.attachmentFragmentShadingRate = VK_TRUE;
    enableFsr.pipelineFragmentShadingRate   = m_bVRSPipelineSupported ? VK_TRUE : VK_FALSE;
    if (m_bVRSSupported) {
        enableFsr.pNext = const_cast<void*>(features13.pNext);
        features13.pNext = &enableFsr;
    }

    // Chain memory-priority feature (prepend, preserving the existing pNext chain).
    VkPhysicalDeviceMemoryPriorityFeaturesEXT enableMemPrio = {};
    enableMemPrio.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT;
    enableMemPrio.memoryPriority = VK_TRUE;
    if (m_bMemoryPrioritySupported) {
        enableMemPrio.pNext = const_cast<void*>(features13.pNext);
        features13.pNext = &enableMemPrio;
    }

    // Device create info
    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &deviceFeatures;
    createInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.enabledExtensionCount = static_cast<u32>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames = enabledExtensions.data();

    // Device-level layers are deprecated and ignored by modern loaders — validation
    // is controlled entirely at the instance (see VK_CreateInstance, gated on the
    // -vk_validation flag). Leave these zero so device creation never pulls the
    // (expensive) validation layer regardless of the instance choice.
    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = nullptr;

    // Создаём device
    VkResult result = vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device);
    if (result != VK_SUCCESS) {
        Msg("!Failed to create logical device. Error: %d", result);
        return false;
    }

    Msg("[Vulkan] Logical device created");
#ifdef __ANDROID__
    xr_vk_android_load_device(m_Device);
#endif

    // Получаем queue handles. When families coincide, vkGetDeviceQueue(fam, 0)
    // legally returns the SAME VkQueue handle, so compute/transfer alias graphics.
    vkGetDeviceQueue(m_Device, m_GraphicsFamily, 0, &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, m_PresentFamily,  0, &m_PresentQueue);
    vkGetDeviceQueue(m_Device, m_ComputeFamily,  0, &m_ComputeQueue);
    vkGetDeviceQueue(m_Device, m_TransferFamily, 0, &m_TransferQueue);

    Msg("[Vulkan] Queue handles obtained (graphics/present/compute/transfer)");

    return true;
}

// Создание VMA
bool CVulkanHW::CreateVMA()
{
    // Явно указываем Vulkan функции для VMA
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    vulkanFunctions.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    vulkanFunctions.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vulkanFunctions.vkAllocateMemory = vkAllocateMemory;
    vulkanFunctions.vkFreeMemory = vkFreeMemory;
    vulkanFunctions.vkMapMemory = vkMapMemory;
    vulkanFunctions.vkUnmapMemory = vkUnmapMemory;
    vulkanFunctions.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    vulkanFunctions.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    vulkanFunctions.vkBindBufferMemory = vkBindBufferMemory;
    vulkanFunctions.vkBindImageMemory = vkBindImageMemory;
    vulkanFunctions.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    vulkanFunctions.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    vulkanFunctions.vkCreateBuffer = vkCreateBuffer;
    vulkanFunctions.vkDestroyBuffer = vkDestroyBuffer;
    vulkanFunctions.vkCreateImage = vkCreateImage;
    vulkanFunctions.vkDestroyImage = vkDestroyImage;
    vulkanFunctions.vkCmdCopyBuffer = vkCmdCopyBuffer;

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    // Let VMA consume the optional streaming extensions we enabled at device
    // creation. BUDGET → vmaGetHeapBudgets returns the driver's live figures;
    // PRIORITY → VmaAllocationCreateInfo::priority is forwarded to the allocation.
    // Both flags are only legal when the matching extension is actually enabled.
#ifdef __SWITCH__
    m_bManualBudget = m_bMemoryBudgetSupported;   // read by hand at level load (RefreshDriverBudget), not by VMA
#endif
    if (m_bMemoryBudgetSupported && !m_bManualBudget) allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    if (m_bMemoryPrioritySupported) allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT;
    allocatorInfo.physicalDevice = m_PhysicalDevice;
    allocatorInfo.device = m_Device;
    allocatorInfo.instance = m_Instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;
    // 64 MB heap blocks instead of VMA's 256 MB default. A block returns to the OS
    // only when its LAST allocation dies; with 256 MB granularity the load-time
    // churn (bake scratch, staging, swapped textures) left blocks pinned by a few
    // survivors — measured 1449 MB of "vma-slack" on Pripyat (17-07 23:30 log), the
    // bulk of the overcommit that starved NGX/DLSS. Finer blocks bound each pin to
    // 64 MB and empty out far more often. Allocation-count cost is trivial
    // (~120 blocks for the whole card + dedicated images, limit 4096).
    allocatorInfo.preferredLargeHeapBlockSize = 64ull << 20;

    VkResult result = vmaCreateAllocator(&allocatorInfo, &m_Allocator);

    if (result != VK_SUCCESS) {
        return false;
    }

    return true;
}

// Sum live usage/budget across all DEVICE_LOCAL heaps. With VK_EXT_memory_budget
// active these are the driver's real figures; otherwise VMA reports the static heap
// size as budget and its own allocation total as usage (still useful, just not
// OS-aware). Called by the texture streamer + ResourcesGetMemoryUsage.
void CVulkanHW::GetVramBudget(VkDeviceSize& outUsage, VkDeviceSize& outBudget) const
{
    outUsage = 0;
    outBudget = 0;
    if (m_Allocator == VK_NULL_HANDLE)
        return;

    const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
    vmaGetMemoryProperties(m_Allocator, &memProps);
    if (!memProps)
        return;

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(m_Allocator, budgets);

    VkDeviceSize blocks = 0;
    for (u32 h = 0; h < memProps->memoryHeapCount; ++h) {
        if (memProps->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            outUsage  += budgets[h].usage;
            outBudget += budgets[h].budget;
            blocks    += budgets[h].statistics.blockBytes;
        }
    }
    if (m_bManualBudget && m_drvBudgetValid) {
        // the driver's figures from the last RefreshDriverBudget, moved by what VMA allocated or freed since
        outUsage  = (blocks >= m_vmaBlocks0) ? m_drvUsage0 + (blocks - m_vmaBlocks0)
                                             : (m_drvUsage0 > m_vmaBlocks0 - blocks ? m_drvUsage0 - (m_vmaBlocks0 - blocks) : 0);
        outBudget = m_drvBudget0;
    }
}

void CVulkanHW::RefreshDriverBudget()
{
    if (!m_bManualBudget || m_Allocator == VK_NULL_HANDLE || m_PhysicalDevice == VK_NULL_HANDLE)
        return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb = {};
    mb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 mp2 = {};
    mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    mp2.pNext = &mb;
    const u64 t0 = CPU::QPC();
    vkGetPhysicalDeviceMemoryProperties2(m_PhysicalDevice, &mp2);
    const double ms = 1000.0 * double(CPU::QPC() - t0) / double(CPU::QPCFreq());

    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(m_Allocator, budgets);
    VkDeviceSize usage = 0, budget = 0, blocks = 0;
    for (u32 h = 0; h < mp2.memoryProperties.memoryHeapCount; ++h) {
        const VkMemoryHeap& heap = mp2.memoryProperties.memoryHeaps[h];
        if (!(heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            continue;
        usage  += mb.heapUsage[h];
        budget += std::min(mb.heapBudget[h], heap.size);
        blocks += budgets[h].statistics.blockBytes;
    }
    m_drvUsage0 = usage;
    m_drvBudget0 = budget;
    m_vmaBlocks0 = blocks;
    m_drvBudgetValid = budget != 0;
    Msg("[Vulkan] memory budget read by hand: usage %llu / budget %llu MB (VMA blocks %llu MB) in %.1f ms",
        (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20), (unsigned long long)(blocks >> 20), ms);
}

// Helper struct for finding unique video modes
struct _uniq_mode_vulkan
{
    LPCSTR _val;
    _uniq_mode_vulkan(LPCSTR v) : _val(v) {}
    bool operator()(LPCSTR _other) { return !_stricmp(_val, _other); }
};

// Free vid_mode_token list (called on shutdown)
static void free_vid_mode_list_vulkan()
{
    if (vid_mode_token == NULL) return;

    for (int i = 0; vid_mode_token[i].name; i++)
    {
        xr_free(vid_mode_token[i].name);
    }
    xr_free(vid_mode_token);
    vid_mode_token = NULL;
    Msg("[Vulkan] Video mode list freed");
}

// Fill vid_mode_token with available display modes
static void fill_vid_mode_list_vulkan()
{
    if (vid_mode_token != NULL) return;

    xr_vector<LPCSTR> _tmp;
    auto addMode = [&_tmp](u32 w, u32 h, u32 bpp) {
        // Filter out small resolutions and non-32bit modes
        if (w < 800 || bpp < 32)
            return;

        string32 str;
        xr_sprintf(str, sizeof(str), "%dx%d", w, h);

        // Check if this mode is already in the list
        if (_tmp.end() != std::find_if(_tmp.begin(), _tmp.end(), _uniq_mode_vulkan(str)))
            return;

        _tmp.push_back(xr_strdup(str));
    };

#ifdef _WIN32
    DEVMODEW dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    // Enumerate all display modes
    for (DWORD iModeNum = 0; EnumDisplaySettingsW(NULL, iModeNum, &dm); iModeNum++)
        addMode(dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel);
#elif defined(__SWITCH__)
    // The Switch panel is 1280x720; docked it outputs up to 1920x1080. Those are the only two modes the
    // compositor accepts for a title, so list them instead of asking SDL's video layer (EGL-oriented).
    addMode(1280, 720, 32);
    addMode(1920, 1080, 32);
#else
    // Modes of the primary display (display 0 is what EnumDisplaySettings(NULL) reports).
    for (int i = 0, n = SDL_GetNumDisplayModes(0); i < n; ++i) {
        SDL_DisplayMode m{};
        if (SDL_GetDisplayMode(0, i, &m) == 0)
            // SDL counts XRGB8888 as 24 bpp (no alpha) — that is the 32-bit desktop mode.
            addMode(u32(m.w), u32(m.h), SDL_BITSPERPIXEL(m.format) >= 24 ? 32u : SDL_BITSPERPIXEL(m.format));
    }
#endif

    // Sort by resolution (width first, then height)
    std::sort(_tmp.begin(), _tmp.end(), [](LPCSTR a, LPCSTR b) {
        int wa, ha, wb, hb;
        sscanf(a, "%dx%d", &wa, &ha);
        sscanf(b, "%dx%d", &wb, &hb);
        if (wa != wb) return wa < wb;
        return ha < hb;
    });

    u32 _cnt = _tmp.size() + 1;
    vid_mode_token = xr_alloc<xr_token>(_cnt);
    vid_mode_token[_cnt - 1].id = -1;
    vid_mode_token[_cnt - 1].name = NULL;

    Msg("[Vulkan] Available video modes[%d]:", _tmp.size());
    for (u32 i = 0; i < _tmp.size(); ++i)
    {
        vid_mode_token[i].id = i;
        vid_mode_token[i].name = _tmp[i];
        Msg("  [%d] %s", i, _tmp[i]);
    }
}

// Главный метод создания device (аналог DX11)
// Returns true on success, false on error
bool CVulkanHW::CreateDevice(HWND hWnd)
{
    Msg("=================================================================");
    Msg("[Vulkan] Initializing Vulkan renderer...");
    Msg("=================================================================");

    m_hWnd = hWnd;

    // ========================================================================
    // Step 0: NVIDIA Streamline — MUST init before ANY Vulkan call (the linked
    // sl.interposer proxies vkCreateInstance/Device so SL can hook DLSS/Reflex/FG).
    // Non-fatal: if slInit fails, SL stays off and the raw-NGX DLSS path is used.
    //
    // ⚠ONLY brought up when something actually needs it (r_dlss_sl / r_dlss_fg).
    // Streamline is a prerequisite for those two routes and for NOTHING else — the
    // default DLSS path is raw NGX. Initialising it unconditionally left its
    // interposer hooking vkCmd* for a plugin set that, on this machine, does not
    // even come up (logs 16-08: "failed to load NGXCore: 126", "no matching adapter
    // found", "Hook sl.common:Vulkan:CmdBindPipeline is NOT supported", plus a 3 s
    // adapter-probe wait at startup and ~380 log lines a run).
    //
    // Reading the cvars HERE is safe: x_ray.cpp executes user.ltx before the render
    // device is created (verified in the startup log — user.ltx at 17:37:10.42,
    // "Initializing Vulkan renderer" at 17:37:10.53). The flip side is that enabling
    // SL needs a RESTART, which CRender::Render reports if r_dlss_sl is set mid-run.
    // `-force_sl` forces it on for a run; `-no_sl` (inside Init) still hard-disables.
    // ========================================================================
    if (ps_r_dlss_sl || ps_r_dlss_fg || strstr(Core.Params, "-force_sl"))
        VK::SL::Init();
    else
        Msg("[VK SL] not requested (r_dlss_sl 0, r_dlss_fg 0) — Streamline NOT initialised; "
            "DLSS runs on the raw-NGX path. Set r_dlss_sl 1 in user.ltx and restart for the SL/FG route.");

    // ========================================================================
    // Step 1: Create Vulkan instance
    // ========================================================================
    Msg("[Vulkan] Step 1/11: Creating Vulkan instance...");
    if (!VK_CreateInstance(&m_Instance)) {
        Msg("![Vulkan] FAILED: Step 1 - Failed to create Vulkan instance");
        Msg("![Vulkan] ");
        Msg("![Vulkan] Possible reasons:");
        Msg("![Vulkan] - Vulkan SDK not installed");
        Msg("![Vulkan] - Outdated GPU drivers");
        Msg("![Vulkan] - Vulkan not supported by system");
        Msg("![Vulkan] ");
        Msg("![Vulkan] Solutions:");
        Msg("![Vulkan] 1. Install Vulkan SDK from https://vulkan.lunarg.com/");
        Msg("![Vulkan] 2. Update GPU drivers to latest version");
        Msg("![Vulkan] 3. Use DirectX renderer with -dx11 flag");
        return false;
    }
    Msg("[Vulkan] Step 1/11: SUCCESS - Vulkan instance created");

    // 2. Debug messenger (validation error/warning output)
    VK_SetupDebugMessenger(m_Instance, &m_DebugMessenger);

    // ========================================================================
    // Step 2: Create window surface
    // ========================================================================
    Msg("[Vulkan] Step 2/11: Creating window surface...");
    VkResult result = VK_CreatePlatformSurface(m_Instance, hWnd, &m_Surface);
    if (result != VK_SUCCESS) {
        Msg("![Vulkan] FAILED: Step 2 - Failed to create window surface (error: %d)", result);
        Msg("![Vulkan] Window handle may be invalid");
        return false;
    }
    Msg("[Vulkan] Step 2/11: SUCCESS - Window surface created");

    // ========================================================================
    // Step 3: Select physical device (GPU)
    // ========================================================================
    Msg("[Vulkan] Step 3/11: Selecting physical device (GPU)...");
    m_PhysicalDevice = VK_SelectPhysicalDevice(m_Instance, &Caps);
    if (m_PhysicalDevice == VK_NULL_HANDLE) {
        Msg("![Vulkan] FAILED: Step 3 - No compatible GPU found");
        Msg("![Vulkan] ");
        Msg("![Vulkan] Possible reasons:");
        Msg("![Vulkan] - No Vulkan-capable GPU detected");
        Msg("![Vulkan] - GPU drivers too old");
        Msg("![Vulkan] - Integrated GPU disabled in BIOS");
        Msg("![Vulkan] ");
        Msg("![Vulkan] Solutions:");
        Msg("![Vulkan] 1. Update GPU drivers to latest version");
        Msg("![Vulkan] 2. Check GPU is enabled in Device Manager");
        Msg("![Vulkan] 3. Use DirectX renderer with -dx11 flag");
        return false;
    }

    // Streamline: probe per-feature support now that the adapter is known.
    VK::SL::OnDeviceReady(m_PhysicalDevice);

    // Display GPU information
    Msg("[Vulkan] Step 3/11: SUCCESS - Physical device selected");
    Msg("[Vulkan] ");
    Msg("[Vulkan] GPU Information:");
    Msg("[Vulkan] ----------------------------------------");
    Msg("[Vulkan] Device Name: %s", Caps.deviceName);
    Msg("[Vulkan] Vulkan API: %d.%d.%d",
        VK_VERSION_MAJOR(Caps.apiVersion),
        VK_VERSION_MINOR(Caps.apiVersion),
        VK_VERSION_PATCH(Caps.apiVersion));
    Msg("[Vulkan] Driver Version: %d.%d.%d",
        VK_VERSION_MAJOR(Caps.driverVersion),
        VK_VERSION_MINOR(Caps.driverVersion),
        VK_VERSION_PATCH(Caps.driverVersion));
    Msg("[Vulkan] ----------------------------------------");
    Msg("[Vulkan] ");

    // ========================================================================
    // Step 4: Check minimum requirements
    // ========================================================================
    Msg("[Vulkan] Step 4/11: Checking GPU capabilities...");
    if (!Caps.CheckMinimumRequirements()) {
        Msg("![Vulkan] FAILED: Step 4 - GPU doesn't meet minimum requirements");
        Msg("![Vulkan] ");
        Msg("![Vulkan] Requirements:");
        Msg("![Vulkan] - Vulkan 1.2 or 1.3");
        Msg("![Vulkan] - VK_KHR_dynamic_rendering extension");
        Msg("![Vulkan] - VK_KHR_synchronization2 extension");
        Msg("![Vulkan] ");
        Msg("![Vulkan] Your GPU:");
        Msg("![Vulkan] - Vulkan API: %d.%d.%d",
            VK_VERSION_MAJOR(Caps.apiVersion),
            VK_VERSION_MINOR(Caps.apiVersion),
            VK_VERSION_PATCH(Caps.apiVersion));
        Msg("![Vulkan] ");
        Msg("![Vulkan] Solutions:");
        Msg("![Vulkan] 1. Update GPU drivers to latest version");
        Msg("![Vulkan] 2. For NVIDIA: Driver 515+ (GTX 1000 series+)");
        Msg("![Vulkan] 3. For AMD: Driver 21.10.1+ (RX 5000 series+)");
        Msg("![Vulkan] 4. For Intel: Driver 30.0.101.1191+ (Arc A-series)");
        Msg("![Vulkan] 5. Use DirectX renderer with -dx11 flag");
        return false;
    }
    Msg("[Vulkan] Step 4/11: SUCCESS - GPU meets all requirements");

    // ========================================================================
    // Step 5: Find queue families
    // ========================================================================
    Msg("[Vulkan] Step 5/11: Finding queue families...");
    if (!FindQueueFamilies()) {
        Msg("![Vulkan] FAILED: Step 5 - Failed to find suitable queue families");
        Msg("![Vulkan] GPU doesn't support required graphics/present queues");
        return false;
    }
    Msg("[Vulkan] Step 5/11: SUCCESS - Queue families found (Graphics: %u, Present: %u)",
        m_GraphicsFamily, m_PresentFamily);

    // ========================================================================
    // Step 6: Create logical device
    // ========================================================================
    Msg("[Vulkan] Step 6/11: Creating logical device...");
    if (!CreateLogicalDevice()) {
        Msg("![Vulkan] FAILED: Step 6 - Failed to create logical device");
        Msg("![Vulkan] This may indicate driver issues or unsupported features");
        return false;
    }
    Msg("[Vulkan] Step 6/11: SUCCESS - Logical device created");

    // Streamline: sl.dlss_g learns the game window (frame-pacing refresh-rate
    // queries) from its vkCreateWin32SurfaceKHR after-hook, but SL plugins only
    // initialise INSIDE vkCreateDevice — the Step-2 surface predates them, so
    // the hook never fired and dlss_g polls a garbage HWND every frame
    // ("Window handle ... is not a valid window" log spam; FG pacing would be
    // blind). Recreate the surface now that the hooks are live; nothing holds
    // the old one yet (the swapchain comes later).
    if (VK::SL::Inited()) {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
        result = VK_CreatePlatformSurface(m_Instance, hWnd, &m_Surface);
        if (result != VK_SUCCESS) {
            Msg("![Vulkan] FAILED: Streamline surface recreate (error: %d)", result);
            return false;
        }
        Msg("[Vulkan] Streamline: window surface recreated post-device (SL surface hook now live)");
    }

    // ========================================================================
    // Step 7: Create VMA (Vulkan Memory Allocator)
    // ========================================================================
    Msg("[Vulkan] Step 7/11: Creating VMA allocator...");
    if (!CreateVMA()) {
        Msg("![Vulkan] FAILED: Step 7 - Failed to create VMA allocator");
        Msg("![Vulkan] Memory allocation system initialization failed");
        return false;
    }
    Msg("[Vulkan] Step 7/11: SUCCESS - VMA allocator created");

    // ========================================================================
    // Step 8: Create command pool for transfer operations
    // ========================================================================
    Msg("[Vulkan] Step 8/11: Creating transfer command pool...");
    VkCommandPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = m_GraphicsFamily;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;  // Short-lived commands

    result = vkCreateCommandPool(m_Device, &poolInfo, nullptr, &m_TransferCommandPool);
    if (result != VK_SUCCESS) {
        Msg("![Vulkan] FAILED: Step 8 - Failed to create transfer command pool (error: %d)", result);
        return false;
    }
    Msg("[Vulkan] Step 8/11: SUCCESS - Transfer command pool created");

    // ========================================================================
    // Step 9 + 10: Geometry + Lighting subsystems — DEFERRED to Stage 2.b.
    // Their .cpp files transitively pull shared xrRender (rvk.h, dsgraph,
    // light.h) which is R4-D3D-specific. Bringing them online needs the
    // framegraph + a CRender that owns dsgraph state.
    // For Stage 2.a (clear-color frame) the swapchain is enough.
    // ========================================================================
#if 0  // STAGE_2B_GEOMETRY_LIGHTING
    Msg("[Vulkan] Step 9/11: Creating geometry system...");
    g_VulkanGeometry = xr_new<VK::CVulkanGeometry>();
    g_VulkanGeometry->Create();
    Msg("[Vulkan] Step 9/11: SUCCESS - Geometry system created");

    Msg("[Vulkan] Step 10/11: Creating lighting manager...");
    g_VulkanLighting = xr_new<VK::CVulkanLighting>();
    g_VulkanLighting->Create();
    Msg("[Vulkan] Step 10/11: SUCCESS - Lighting manager created");
#else
    Msg("[Vulkan] Step 9+10: SKIPPED (Stage 2.a — geometry/lighting deferred)");
#endif

    // ========================================================================
    // Step 11: Fill video mode list for options menu
    // ========================================================================
    Msg("[Vulkan] Step 11/11: Filling video mode list...");
    fill_vid_mode_list_vulkan();
    Msg("[Vulkan] Step 11/11: SUCCESS - Video mode list filled");

    Msg("=================================================================");
    Msg("[Vulkan] Initialization completed successfully!");
    Msg("[Vulkan] All systems ready");
    Msg("=================================================================");

    // Build provenance / fingerprint banner (decoded from vk_authorship).
    ogsr::sig::Register();

    return true;
}

// Уничтожение device
void CVulkanHW::DestroyDevice()
{
    if (m_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_Device);
    }

    // Free video mode list
    free_vid_mode_list_vulkan();

    // Stage 2.a: paired with the create-side #if 0 above. Re-enable for 2.b.
#if 0  // STAGE_2B_GEOMETRY_LIGHTING
    if (g_VulkanLighting) { xr_delete(g_VulkanLighting); g_VulkanLighting = nullptr; }
    if (g_VulkanGeometry) { xr_delete(g_VulkanGeometry); g_VulkanGeometry = nullptr; }
#endif

    // Уничтожаем command pool
    if (m_TransferCommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_Device, m_TransferCommandPool, nullptr);
        m_TransferCommandPool = VK_NULL_HANDLE;
        Msg("[Vulkan] Transfer command pool destroyed");
    }

    // Уничтожаем VMA
    if (m_Allocator != VK_NULL_HANDLE) {
        VK::Vram::DestroySmallPools(m_Allocator);   // custom pools must die first
        vmaDestroyAllocator(m_Allocator);
        m_Allocator = VK_NULL_HANDLE;
        Msg("[Vulkan] VMA allocator destroyed");
    }

    // Уничтожаем device
    if (m_Device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
        Msg("[Vulkan] Logical device destroyed");
    }

    // Уничтожаем surface
    if (m_Surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
        Msg("[Vulkan] Surface destroyed");
    }

    // Destroy debug messenger
    if (m_DebugMessenger != VK_NULL_HANDLE) {
        VK_DestroyDebugMessenger(m_Instance, m_DebugMessenger);
        m_DebugMessenger = VK_NULL_HANDLE;
    }

    // Уничтожаем instance
    if (m_Instance != VK_NULL_HANDLE) {
        VK_DestroyInstance(m_Instance);
        m_Instance = VK_NULL_HANDLE;
    }

    Msg("[Vulkan] Device destruction complete");
}

// Reset device (при resize окна и т.д.)
void CVulkanHW::Reset(HWND hWnd)
{
    Msg("[Vulkan] Reset device");

    // Ждём завершения всех операций
    if (m_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_Device);
    }

    // TODO: Пересоздать swapchain и render targets
}

// App activation
void CVulkanHW::OnAppActivate()
{
    Msg("[Vulkan] App activated");
}

// App deactivation
void CVulkanHW::OnAppDeactivate()
{
    Msg("[Vulkan] App deactivated");
}

// ============================================================================
// Single-time command helpers (for buffer uploads, layout transitions, etc.)
// ============================================================================

// These now delegate to CommandManager's immediate (one-shot) path instead of
// hand-rolling a second submit-and-wait. That path is device-lost guarded, waits
// on a fence (not a full vkQueueWaitIdle of the whole graphics queue), and uses a
// graphics-family pool — the old code allocated from m_TransferCommandPool yet
// submitted to m_GraphicsQueue, a queue-family mismatch on GPUs with a dedicated
// transfer queue. All callers are load-time + main-thread, so reusing the single
// shared immediate buffer is not reentrant here.
VkCommandBuffer CVulkanHW::BeginSingleTimeCommands()
{
    return CommandManager.BeginImmediate();
}

void CVulkanHW::EndSingleTimeCommands(VkCommandBuffer commandBuffer)
{
    CommandManager.EndAndSubmitImmediate(commandBuffer);
}
