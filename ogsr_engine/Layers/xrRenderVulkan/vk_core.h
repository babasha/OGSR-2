// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once
#include "stdafx.h"

// Global device-lost flag — when set, all rendering is skipped
// Defined in rvk.cpp, declared extern here so it's accessible from xrEngine too
extern bool g_bDeviceLost;

// Triple-buffering depth — single source of truth for the renderer.
// Per-frame GPU resources (command buffers, sync, descriptor pools, ring
// allocators) all size their slot arrays from this. Classes alias it as
// their own static FRAMES_IN_FLIGHT so existing Class::FRAMES_IN_FLIGHT
// references keep working; change the value only here.
inline constexpr u32 VK_FRAMES_IN_FLIGHT = 3;

// Soft error checking macro — logs error, sets device-lost on fatal codes, but does NOT crash
#define VK_CHECK(result) \
    do { \
        VkResult res = (result); \
        if (res != VK_SUCCESS) { \
            if (res == VK_ERROR_DEVICE_LOST) { \
                if (!g_bDeviceLost) { \
                    Msg("!Vulkan DEVICE LOST at %s:%d", __FILE__, __LINE__); \
                    g_bDeviceLost = true; \
                } \
            } else { \
                Msg("!Vulkan error: %d at %s:%d", res, __FILE__, __LINE__); \
            } \
        } \
    } while(0)

// Critical error checking — used for resource creation that MUST succeed
#define VK_CHECK_CRITICAL(result) \
    do { \
        VkResult res = (result); \
        if (res != VK_SUCCESS) { \
            Msg("!Vulkan CRITICAL error: %d at %s:%d", res, __FILE__, __LINE__); \
            VERIFY(false); \
        } \
    } while(0)

// Required instance extensions (debug_utils always included for validation diagnostics).
// Off Windows the window-system surface extension (xlib/xcb/wayland) is only known
// at run time — VK_BaseInstanceExtensions() appends what SDL reports.
inline const char* g_InstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
    VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(__SWITCH__)
    VK_NN_VI_SURFACE_EXTENSION_NAME, // Horizon: the surface comes from libnx's NWindow, not from SDL
#endif
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
};

inline constexpr u32 g_InstanceExtensionCount = sizeof(g_InstanceExtensions) / sizeof(g_InstanceExtensions[0]);

// Validation layers (always available — essential for Vulkan development)
inline const char* g_ValidationLayers[] = {
    "VK_LAYER_KHRONOS_validation"
};
inline constexpr u32 g_ValidationLayerCount = sizeof(g_ValidationLayers) / sizeof(g_ValidationLayers[0]);

// Required device extensions
inline const char* g_DeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// Optional device extensions for NVIDIA NGX (DLSS)
inline const char* g_NgxDeviceExtensions[] = {
    VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
    VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
};
inline constexpr u32 g_NgxDeviceExtensionCount = sizeof(g_NgxDeviceExtensions) / sizeof(g_NgxDeviceExtensions[0]);

// Set to true by CreateLogicalDevice() if all NGX extensions were enabled
inline bool g_bNgxExtensionsEnabled = false;

// Optional device extensions for hardware ray tracing (RTGI)
inline const char* g_RtDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};
inline constexpr u32 g_RtDeviceExtensionCount = sizeof(g_RtDeviceExtensions) / sizeof(g_RtDeviceExtensions[0]);

inline constexpr u32 g_DeviceExtensionCount = sizeof(g_DeviceExtensions) / sizeof(g_DeviceExtensions[0]);

// Forward declarations
struct VulkanCaps;

// Core functions
bool VK_CreateInstance(VkInstance* outInstance);
void VK_DestroyInstance(VkInstance instance);

// Platform seam: the extension list every instance is created with
// (g_InstanceExtensions + the window-system one off Windows), and the window
// surface for hWnd (a real HWND on Windows, the SDL_Window* elsewhere).
const std::vector<const char*>& VK_BaseInstanceExtensions();
VkResult VK_CreatePlatformSurface(VkInstance instance, HWND hWnd, VkSurfaceKHR* outSurface);

// Physical device selection
VkPhysicalDevice VK_SelectPhysicalDevice(VkInstance instance, VulkanCaps* outCaps);
bool VK_CheckDeviceExtensionSupport(VkPhysicalDevice device);

// Debug messenger setup (always available for validation diagnostics)
VkResult VK_SetupDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT* outMessenger);
void VK_DestroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger);
