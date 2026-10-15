// Android: the NDK's libvulkan.so for the API level the build targets (29) exports the Vulkan 1.1 entry points
// only; the platform loader gained the 1.2/1.3 ones as exported symbols in API 33. The renderer calls them
// directly (no volk / VK_NO_PROTOTYPES), so they are defined here as trampolines to the device's own
// functions, fetched once right after vkCreateDevice (HW_Vulkan.cpp). The device is created with 1.3 (the
// renderer refuses a lower one), so the core names resolve; the KHR names are a fallback for odd drivers.
// Not in the .vcxproj: CMake adds it for Android only.
#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

#define XR_VK_ENTRIES(X)                                                                                                  \
    X(vkWaitSemaphores, PFN_vkWaitSemaphores)                                                                             \
    X(vkGetDeviceImageMemoryRequirements, PFN_vkGetDeviceImageMemoryRequirements)                                         \
    X(vkGetBufferDeviceAddress, PFN_vkGetBufferDeviceAddress)                                                             \
    X(vkCmdPipelineBarrier2, PFN_vkCmdPipelineBarrier2)                                                                   \
    X(vkCmdBeginRendering, PFN_vkCmdBeginRendering)                                                                       \
    X(vkCmdEndRendering, PFN_vkCmdEndRendering)                                                                           \
    X(vkCmdDrawIndirectCount, PFN_vkCmdDrawIndirectCount)                                                                 \
    X(vkCmdDrawIndexedIndirectCount, PFN_vkCmdDrawIndexedIndirectCount)

namespace
{
#define XR_VK_DECLARE(name, pfn) pfn p_##name = nullptr;
XR_VK_ENTRIES(XR_VK_DECLARE)
#undef XR_VK_DECLARE

// Called through a null pointer = a feature the device was created without: say which one before dying.
[[noreturn]] void missing(const char* name)
{
    fprintf(stderr, "[vk android] %s is not available on this device\n", name);
    abort();
}
} // namespace

extern "C" void xr_vk_android_load_device(VkDevice device)
{
#define XR_VK_LOAD(name, pfn)                                                                                             \
    p_##name = reinterpret_cast<pfn>(vkGetDeviceProcAddr(device, #name));                                                 \
    if (!p_##name)                                                                                                        \
        p_##name = reinterpret_cast<pfn>(vkGetDeviceProcAddr(device, #name "KHR"));                                       \
    if (!p_##name)                                                                                                        \
        fprintf(stderr, "[vk android] no %s (nor %sKHR)\n", #name, #name);
    XR_VK_ENTRIES(XR_VK_LOAD)
#undef XR_VK_LOAD
}

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkWaitSemaphores(VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout)
{
    if (!p_vkWaitSemaphores) missing("vkWaitSemaphores");
    return p_vkWaitSemaphores(device, pWaitInfo, timeout);
}
VKAPI_ATTR void VKAPI_CALL vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo,
                                                              VkMemoryRequirements2* pMemoryRequirements)
{
    if (!p_vkGetDeviceImageMemoryRequirements) missing("vkGetDeviceImageMemoryRequirements");
    p_vkGetDeviceImageMemoryRequirements(device, pInfo, pMemoryRequirements);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo)
{
    if (!p_vkGetBufferDeviceAddress) missing("vkGetBufferDeviceAddress");
    return p_vkGetBufferDeviceAddress(device, pInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* pDependencyInfo)
{
    if (!p_vkCmdPipelineBarrier2) missing("vkCmdPipelineBarrier2");
    p_vkCmdPipelineBarrier2(cmd, pDependencyInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* pRenderingInfo)
{
    if (!p_vkCmdBeginRendering) missing("vkCmdBeginRendering");
    p_vkCmdBeginRendering(cmd, pRenderingInfo);
}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd)
{
    if (!p_vkCmdEndRendering) missing("vkCmdEndRendering");
    p_vkCmdEndRendering(cmd);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                  VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
    if (!p_vkCmdDrawIndirectCount) missing("vkCmdDrawIndirectCount");
    p_vkCmdDrawIndirectCount(cmd, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
                                                         VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride)
{
    if (!p_vkCmdDrawIndexedIndirectCount) missing("vkCmdDrawIndexedIndirectCount");
    p_vkCmdDrawIndexedIndirectCount(cmd, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}
} // extern "C"
#endif // __ANDROID__
