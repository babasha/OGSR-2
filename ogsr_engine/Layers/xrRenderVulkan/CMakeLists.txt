# Linux build of xrRender_Vulkan. Source list is generated from xrRender_Vulkan.vcxproj — see
# CMakeSources.cmake and cmake/gen_module_sources.sh. Keep in sync with the Windows project;
# do not add sources here.
include(${CMAKE_CURRENT_SOURCE_DIR}/CMakeSources.cmake)

# Vulkan headers + loader come from the distro (Arch: `vulkan-headers`, `vulkan-icd-loader`),
# same call as OpenAL in xrSound: on Windows they come from the LunarG SDK, which isn't a
# thing on Linux. The renderer calls vk* entry points directly (no volk / VK_NO_PROTOTYPES),
# so it links the loader.
find_package(Vulkan REQUIRED)
# Window system on Linux = SDL2 (HWND is SDL_Window*, see vk_core.cpp VK_CreatePlatformSurface).
# Arch ships SDL2 as `sdl2-compat` (SDL2 API on top of SDL3); the includes are <SDL2/SDL.h>.
if(NOT OGSR_SWITCH AND NOT ANDROID) # Android: SDL2::SDL2 is built from source (top-level CMakeLists.txt)
    find_package(SDL2 REQUIRED)
endif()

add_library(xrRenderVulkan STATIC ${XRRENDERVULKAN_SOURCES})
if(ANDROID)
    # Vulkan 1.2/1.3 entry points the NDK's libvulkan.so (API 29) does not export -- see the file header.
    target_sources(xrRenderVulkan PRIVATE vk_android_entry.cpp)
endif()
if(OGSR_SWITCH)
    # devkitPro's SDL2 config exports .../include/SDL2; the renderer writes <SDL2/SDL.h>.
    target_include_directories(xrRenderVulkan PRIVATE $ENV{DEVKITPRO}/portlibs/switch/include)
endif()

target_include_directories(xrRenderVulkan PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(xrRenderVulkan PRIVATE
    ${OGSR_3RD_PARTY_DIR}/VulkanMemoryAllocator/VulkanMemoryAllocator/include
    # Headers only. What actually links on Linux (resolved at the xr_3da link, not here):
    #  - Streamline: Windows-only; vk_sl.cpp reports "SL unavailable" on Linux, no symbols.
    #  - DLSS/NGX: the SDK ships lib/Linux_x86_64/libnvsdk_ngx.a (x86_64 only, not arm64).
    #  - FidelityFX (FSR3 upscaler + VK backend): only Windows .lib via #pragma comment in
    #    vk_fsr.cpp; no Linux build of the SDK yet.
    ${OGSR_3RD_PARTY_DIR}/NVIDIA_Streamline/include
    ${OGSR_3RD_PARTY_DIR}/NVIDIA_DLSS/DLSS/include
    ${OGSR_3RD_PARTY_DIR}/AMD_FidelityFX/sdk/include
)

target_compile_definitions(xrRenderVulkan PUBLIC
    XRRENDER_STATIC
)
target_compile_definitions(xrRenderVulkan PRIVATE
    NDEBUG
)
# _WINDOWS/_USRDLL are in the vcxproj but unused anywhere under Layers/ (no grep hits), same
# call as the other modules: left undefined for the Linux target.

target_link_libraries(xrRenderVulkan PUBLIC
    xrCore
    xrCDB
    xrParticles
    Luabind
    Vulkan::Vulkan
    $<$<NOT:$<BOOL:${OGSR_SWITCH}>>:SDL2::SDL2>
)

if(OGSR_SWITCH)
    # NVK's libvulkan.a needs these (Mesa's xmlconfig -> expat, shader cache -> zlib/zstd). Kept as plain
    # link items (not imported targets) so they land AFTER libvulkan.a on the link line. No SDL library:
    # the window is libnx's NWindow (VK_NN_vi_surface) and input is libnx HID; linking SDL2 would drag in
    # portlibs' libEGL/libGLESv2, which carry Mesa 20.1's copy of the same util code as NVK's Mesa 26.
    target_link_libraries(xrRenderVulkan PUBLIC expat zstd z)
endif()
