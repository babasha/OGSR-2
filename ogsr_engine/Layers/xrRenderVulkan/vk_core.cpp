// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#include "stdafx.h"
#include "vk_core.h"
#include "HWCaps_Vulkan.h"
#include <vector>
#include <cstdlib>   // _putenv_s — VK_LAYER_DUPLICATE_MESSAGE_LIMIT fallback
#include <algorithm>
#ifndef _WIN32
#  include <SDL2/SDL.h>
#  include <SDL2/SDL_vulkan.h>
#endif

// Debug messenger callback (always compiled — validation is essential for Vulkan dev)
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    const char* severity = "INFO";
    if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        severity = "ERROR";
    } else if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        severity = "WARNING";
    }

    // Log message ID for easy filtering (e.g. VUID-vkCmdDraw-...)
    if (pCallbackData->pMessageIdName)
        Msg("[VK-VAL %s] [%s] %s", severity, pCallbackData->pMessageIdName, pCallbackData->pMessage);
    else
        Msg("[VK-VAL %s] %s", severity, pCallbackData->pMessage);

    // FlushLog() existed in monolith xrCore; OGSR removed it. Validation
    // messages are still queued by Msg() — the engine flushes log on its own
    // schedule (file flushed on shutdown / crash dump).
    return VK_FALSE;
}

bool VK_CreateInstance(VkInstance* outInstance)
{
    VERIFY(outInstance);

    // Application info
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "X-Ray Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "X-Ray";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    // Instance create info
    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = u32(VK_BaseInstanceExtensions().size());
    createInfo.ppEnabledExtensionNames = VK_BaseInstanceExtensions().data();

    // Check if validation layer is actually installed on this system
    u32 layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    bool validationAvailable = false;
    for (const auto& layer : availableLayers) {
        if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            validationAvailable = true;
            break;
        }
    }

    // Validation is OFF by default — the KHRONOS layer realistically costs 2-10x
    // (NOT the "~5%" the old message claimed), and every error also runs through
    // the debug callback → Msg → synchronous log write. Opt in with the
    // `-vk_validation` command-line flag when debugging barriers/descriptors/layouts.
    const bool wantValidation = Core.Params && strstr(Core.Params, "-vk_validation");
    validationAvailable = validationAvailable && wantValidation;

    // Debug messenger for instance creation/destruction messages
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = {};
    debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = DebugCallback;

    // --- Layer settings (VK_EXT_layer_settings) --------------------------------
    // The default layer configuration answers a DIFFERENT question than the one we
    // run validation for, in two ways that both read as "clean" when they are not:
    //
    // 1. SYNCHRONIZATION validation is OFF by default. Core validation checks that
    //    the barriers we DID write are well-formed; it never checks for a barrier
    //    we FORGOT. A hazard — compute writes an SSBO, the draw reads it with
    //    nothing in between — is invisible to it. So every "no sync errors" run
    //    before this one proved nothing whatsoever about barrier correctness: the
    //    layer was not looking. `validate_sync` is the setting that looks, and it
    //    is the entire reason this renderer runs validation at all.
    // 2. `duplicate_message_limit` defaults to 10: a VUID goes SILENT after ten
    //    reports. A capped count reads as a total and is not one (the 16-08 run had
    //    four separate VUIDs all reporting exactly 10 — i.e. "at least 10, actual
    //    unknown"). Raised so the counts mean something.
    //
    // Sync validation is heavy even by validation standards (it tracks every
    // access of every resource). `-vk_nosyncval` keeps the cheap core-only checks
    // for when a run has to be long enough to reach some late game state.
    const bool wantSyncVal = wantValidation && !(Core.Params && strstr(Core.Params, "-vk_nosyncval"));

    // Which mechanism this machine's layer actually offers. VK_EXT_layer_settings
    // is the modern one and supersedes VK_EXT_validation_features — but it ships
    // with the LAYER, and the layer installed here can be older than the SDK
    // headers we compile against. Asking for an absent instance extension makes
    // vkCreateInstance fail outright with VK_ERROR_EXTENSION_NOT_PRESENT (-7),
    // i.e. a diagnostic flag that bricks startup. So: enumerate the extensions the
    // validation layer itself exposes, and use whichever exists.
    bool haveLayerSettings = false, haveValidationFeatures = false;
    if (validationAvailable) {
        u32 n = 0;
        vkEnumerateInstanceExtensionProperties(g_ValidationLayers[0], &n, nullptr);
        std::vector<VkExtensionProperties> layerExts(n);
        if (n) vkEnumerateInstanceExtensionProperties(g_ValidationLayers[0], &n, layerExts.data());
        for (const auto& e : layerExts) {
            if (strcmp(e.extensionName, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME) == 0)       haveLayerSettings = true;
            if (strcmp(e.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0)  haveValidationFeatures = true;
        }
    }

    // ⚠⚠THE EXTENSION ROUTE IS NOT ENOUGH — measured 16-08 18:35. The layer DOES
    // report VK_EXT_layer_settings from vkEnumerateInstanceExtensionProperties, we
    // chained it, and vkCreateInstance still returned -7 EXTENSION_NOT_PRESENT: the
    // loader resolves a DIFFERENT (older) validation layer manifest than the one
    // that answered the enumeration. The retry-without-extras path then saved the
    // launch — and silently took sync validation with it. Net effect: the log said
    // "SYNCHRONIZATION validation ON" on a run where it was off, which is the exact
    // failure mode this whole block exists to prevent.
    //
    // So ALSO configure the layer the way that needs no instance extension at all:
    // environment variables, which the layer reads with getenv at instance creation
    // and which therefore survive the fallback instance too. Belt and braces — the
    // extension attempt stays (it is the documented route), the env vars are what
    // actually has to work. Unknown names are ignored by the layer, so setting both
    // the legacy and the modern spelling costs nothing.
    //   VK_LAYER_ENABLES                        legacy, understood by every layer build
    //   VK_KHRONOS_VALIDATION_VALIDATE_SYNC     modern per-layer setting env
    // ⭐_putenv_s, NOT SetEnvironmentVariable: the layer's getenv reads the CRT's own
    // copy of the environment block, which the Win32 call does not update.
    // ⭐VK_LAYER_VALIDATE_SYNC is the CURRENT name — the layer itself said so in the
    // 22:25 run ("Deprecated: VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
    // | New: validate_sync (VK_LAYER_VALIDATE_SYNC=1)"), which is also the proof that it
    // read these vars at all. VK_LAYER_ENABLES stays as the fallback for older layers.
    if (wantSyncVal) {
        _putenv_s("VK_LAYER_VALIDATE_SYNC", "1");
        _putenv_s("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
        _putenv_s("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
        _putenv_s("VK_LAYER_DUPLICATE_MESSAGE_LIMIT", "5000");
        _putenv_s("VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT", "5000");
    }

    // Storage for both mechanisms must outlive vkCreateInstance — function scope.
    static const VkBool32 kSyncValOn   = VK_TRUE;
    static const u32      kDupMsgLimit = 5000;
    const VkLayerSettingEXT layerSettings[] = {
        { "VK_LAYER_KHRONOS_validation", "validate_sync",
          VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kSyncValOn },
        { "VK_LAYER_KHRONOS_validation", "duplicate_message_limit",
          VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &kDupMsgLimit },
    };
    VkLayerSettingsCreateInfoEXT layerSettingsInfo = {};
    layerSettingsInfo.sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layerSettingsInfo.settingCount = u32(sizeof(layerSettings) / sizeof(layerSettings[0]));
    layerSettingsInfo.pSettings    = layerSettings;

    const VkValidationFeatureEnableEXT enableSync[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT validationFeatures = {};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures    = enableSync;

    std::vector<const char*> instanceExts(VK_BaseInstanceExtensions());

    const char* syncMechanism = nullptr;   // set below; the verdict is printed after creation
    if (validationAvailable) {
        createInfo.enabledLayerCount = g_ValidationLayerCount;
        createInfo.ppEnabledLayerNames = g_ValidationLayers;

        if (wantSyncVal && haveLayerSettings) {
            instanceExts.push_back(VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
            debugCreateInfo.pNext = &layerSettingsInfo;
            syncMechanism = "VK_EXT_layer_settings";
        } else if (wantSyncVal && haveValidationFeatures) {
            instanceExts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            debugCreateInfo.pNext = &validationFeatures;
            syncMechanism = "VK_EXT_validation_features";
            // That older mechanism carries no message-limit knob; the layer reads
            // this env var for it. _putenv_s (not SetEnvironmentVariable) because
            // the layer's getenv goes through the CRT's own copy of the block.
            char lim[16]; xr_sprintf(lim, "%u", kDupMsgLimit);
            _putenv_s("VK_LAYER_DUPLICATE_MESSAGE_LIMIT", lim);
        }
        createInfo.enabledExtensionCount   = u32(instanceExts.size());
        createInfo.ppEnabledExtensionNames = instanceExts.data();
        createInfo.pNext = &debugCreateInfo;

        Msg("[Vulkan] Validation layers ENABLED (-vk_validation) — expect a large FPS hit");
        Msg("[Vulkan] Barrier/descriptor/layout errors will appear as [VK-VAL] in log");
        // ⚠Do NOT announce sync validation here: whether the chained mechanism
        // survives is only known after vkCreateInstance (it did not, 16-08 18:35).
        // The verdict is printed below, once the instance actually exists.
        if (syncMechanism)
            Msg("[Vulkan] requesting sync validation via %s + env (verdict after instance creation)", syncMechanism);
        else if (wantSyncVal)
            Msg("[Vulkan] requesting sync validation via env only (layer exposes neither layer_settings nor validation_features)");
        else
            Msg("[Vulkan] Synchronization validation OFF (-vk_nosyncval) — MISSING barriers will NOT be reported");
    } else {
        createInfo.enabledLayerCount = 0;
        createInfo.pNext = nullptr;
        Msg("[Vulkan] Validation layers OFF (pass -vk_validation to enable diagnostics)");
    }

    // Создаём instance
    VkResult result = vkCreateInstance(&createInfo, nullptr, outInstance);

    // A DIAGNOSTIC must never be able to prevent the game from starting. If the
    // extras we chained on for validation are what the loader rejected, drop them
    // and create a plain instance rather than failing the whole renderer — that is
    // exactly what a stale layer did here once (-7 EXTENSION_NOT_PRESENT), and it
    // presented as "Vulkan not supported by system".
    bool extensionRouteLost = false;
    if (result != VK_SUCCESS && validationAvailable && instanceExts.size() > VK_BaseInstanceExtensions().size()) {
        Msg("![Vulkan] instance creation failed (%d) WITH the validation extras — retrying without them", result);
        createInfo.enabledExtensionCount   = u32(VK_BaseInstanceExtensions().size());
        createInfo.ppEnabledExtensionNames = VK_BaseInstanceExtensions().data();
        debugCreateInfo.pNext = nullptr;
        result = vkCreateInstance(&createInfo, nullptr, outInstance);
        extensionRouteLost = (result == VK_SUCCESS);
    }

    if (result != VK_SUCCESS) {
        Msg("!Failed to create Vulkan instance. Error code: %d", result);
        return false;
    }

    // The sync-validation verdict, printed only now that the instance exists — and
    // stated as what it is: a REQUEST, not a confirmation. The layer offers no way
    // to read back which checks it enabled, so the only honest proof that sync
    // validation ran is SYNC-HAZARD-* lines actually appearing in this log.
    // ⚠A run with zero SYNC-HAZARD lines does NOT mean the barriers are correct —
    // it means either that, or that the instrument was off again. Do not repeat the
    // 16-08 mistake of reading silence as a clean bill of health.
    if (validationAvailable && wantSyncVal) {
        if (extensionRouteLost)
            Msg("~[Vulkan] the %s route was REJECTED by the loader — sync validation now rides on the "
                "env vars alone (VK_LAYER_ENABLES / VK_KHRONOS_VALIDATION_VALIDATE_SYNC)",
                syncMechanism ? syncMechanism : "extension");
        Msg("[Vulkan] sync validation REQUESTED (%s + env). PROOF = SYNC-HAZARD-* lines below; "
            "their ABSENCE proves nothing about barriers.",
            extensionRouteLost ? "extension rejected" : (syncMechanism ? syncMechanism : "env only"));
    }

    Msg("[Vulkan] Instance created successfully");
    return true;
}

const std::vector<const char*>& VK_BaseInstanceExtensions()
{
    static const std::vector<const char*> exts = [] {
        std::vector<const char*> v(g_InstanceExtensions, g_InstanceExtensions + g_InstanceExtensionCount);
#if !defined(_WIN32) && !defined(__SWITCH__)
        // SDL knows which window system it runs on (X11 or Wayland) and so which
        // *_surface extension the loader has to enable. The window argument is
        // optional since SDL 2.0.8.
        unsigned int n = 0;
        if (SDL_Vulkan_GetInstanceExtensions(nullptr, &n, nullptr) && n) {
            std::vector<const char*> sdl(n);
            SDL_Vulkan_GetInstanceExtensions(nullptr, &n, sdl.data());
            for (const char* e : sdl)
                if (std::none_of(v.begin(), v.end(), [e](const char* x) { return strcmp(x, e) == 0; }))
                    v.push_back(e);
        } else {
            Msg("![Vulkan] SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        }
#endif
        return v;
    }();
    return exts;
}

#ifdef __SWITCH__
extern "C" void* nwindowGetDefault(void); // libnx: NWindow* of this title's compositor layer
#endif

VkResult VK_CreatePlatformSurface(VkInstance instance, HWND hWnd, VkSurfaceKHR* outSurface)
{
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.hinstance = GetModuleHandle(nullptr);
    surfaceInfo.hwnd = hWnd;
    return vkCreateWin32SurfaceKHR(instance, &surfaceInfo, nullptr, outSurface);
#elif defined(__SWITCH__)
    // Horizon has ONE window: libnx's default NWindow (the compositor layer of this title). NVK's
    // WSI backend (VK_NN_vi_surface) wraps it; hWnd is unused. Declared here rather than including
    // <switch.h>, whose type/macro soup collides with the engine's (Result, Handle, bool...).
    VkViSurfaceCreateInfoNN surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    surfaceInfo.window = nwindowGetDefault();
    (void)hWnd;
    return vkCreateViSurfaceNN(instance, &surfaceInfo, nullptr, outSurface);
#else
    if (SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(hWnd), instance, outSurface))
        return VK_SUCCESS;
    Msg("![Vulkan] SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
    return VK_ERROR_INITIALIZATION_FAILED;
#endif
}

void VK_DestroyInstance(VkInstance instance)
{
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        Msg("[Vulkan] Instance destroyed");
    }
}

VkResult VK_SetupDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT* outMessenger)
{
    VERIFY(outMessenger);

    VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = DebugCallback;

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");

    if (func != nullptr) {
        VkResult result = func(instance, &createInfo, nullptr, outMessenger);
        if (result == VK_SUCCESS) {
            Msg("[Vulkan] Debug messenger created (validation errors will appear in log)");
        }
        return result;
    } else {
        Msg("[Vulkan] vkCreateDebugUtilsMessengerEXT not available");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void VK_DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
{
    if (messenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");

        if (func != nullptr) {
            func(instance, messenger, nullptr);
            Msg("[Vulkan] Debug messenger destroyed");
        }
    }
}

// Проверка поддержки расширений устройства
bool VK_CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
    u32 extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    // Проверяем все требуемые расширения
    for (u32 i = 0; i < g_DeviceExtensionCount; i++) {
        bool found = false;
        for (const auto& extension : availableExtensions) {
            if (strcmp(g_DeviceExtensions[i], extension.extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

// Выбор физического устройства
VkPhysicalDevice VK_SelectPhysicalDevice(VkInstance instance, VulkanCaps* outCaps)
{
    VERIFY(outCaps);

    u32 deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        Msg("!Failed to find GPUs with Vulkan support");
        return VK_NULL_HANDLE;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    // Система оценки устройств
    struct DeviceScore {
        VkPhysicalDevice device;
        int score;
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceVulkan13Features features13;
    };
    std::vector<DeviceScore> scores;

    for (auto device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        // Проверка версии API
        if (props.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }

        // Проверка расширений
        if (!VK_CheckDeviceExtensionSupport(device)) {
            continue;
        }

        // Проверка Vulkan 1.3 features
        VkPhysicalDeviceVulkan13Features features13 = {};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

        VkPhysicalDeviceFeatures2 features2 = {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features13;

        vkGetPhysicalDeviceFeatures2(device, &features2);

        // Проверяем обязательные features
        if (!features13.dynamicRendering || !features13.synchronization2) {
            continue;
        }

        // Подсчет очков
        int score = 0;

        // Discrete GPU +1000
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            score += 1000;
        }

        // Vendor preference
        switch (props.vendorID) {
        case 0x10DE:  // NVIDIA
            score += 100;
            Msg("[Vulkan] Found NVIDIA: %s", props.deviceName);
            break;
        case 0x1002:  // AMD
            score += 90;
            Msg("[Vulkan] Found AMD: %s", props.deviceName);
            break;
        case 0x8086:  // Intel
            score += 50;
            Msg("[Vulkan] Found Intel: %s", props.deviceName);
            break;
        default:
            Msg("[Vulkan] Found GPU: %s (Vendor: 0x%X)", props.deviceName, props.vendorID);
            break;
        }

        // VRAM bonus
        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(device, &memProps);
        VkDeviceSize totalVRAM = 0;
        for (u32 i = 0; i < memProps.memoryHeapCount; i++) {
            if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                totalVRAM += memProps.memoryHeaps[i].size;
            }
        }
        score += static_cast<int>(totalVRAM / (1024ULL * 1024 * 1024));  // +1 per GB

        scores.push_back({ device, score, props, features13 });
    }

    if (scores.empty()) {
        Msg("!No suitable Vulkan GPU found");
        return VK_NULL_HANDLE;
    }

    // Сортировка по score
    std::sort(scores.begin(), scores.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });

    // Выбираем лучшее устройство
    DeviceScore& best = scores[0];

    // ========================================================================
    // Query full features for Update()
    // ========================================================================
    VkPhysicalDeviceFeatures baseFeatures;
    vkGetPhysicalDeviceFeatures(best.device, &baseFeatures);

    // ========================================================================
    // Fill capabilities using Update()
    // ========================================================================
    outCaps->Update(best.device, best.props, baseFeatures, best.features13);

    // ========================================================================
    // Calculate VRAM (not done in Update())
    // ========================================================================
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(best.device, &memProps);
    outCaps->totalDeviceMemory = 0;  // Reset before counting
    for (u32 i = 0; i < memProps.memoryHeapCount; i++) {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            outCaps->totalDeviceMemory += memProps.memoryHeaps[i].size;
        }
    }

    Msg("[Vulkan] Selected: %s (score: %d)", best.props.deviceName, best.score);

    // ========================================================================
    // Print full capabilities report
    // ========================================================================
    outCaps->LogInfo();

    return best.device;
}
