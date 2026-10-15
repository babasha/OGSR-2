// vk_hzb_readback.cpp -- see vk_hzb_readback.h.
#include "stdafx.h"
#include "vk_hzb_readback.h"
#include "vk_buffer.h"        // CVulkanBuffer -- the readback ring
#include "vk_render_view.h"   // RV().dwTimeContinual -- when the copied frame was rendered
#include "HW_Vulkan.h"
#include "../../xr_3da/device.h"   // Device.dwTimeContinual (main thread)
#include <mutex>

namespace VK::HzbReadback
{
namespace
{
// Ring of copies; a slot is read back when it comes round again, kSlots frames later (>= frames in flight, the same
// lag argument as the exposure ring in vk_pass_tonemap.cpp).
constexpr u32 kSlots = 4;
// The finest mip with both sides <= 256: 160x90 at 720p (one texel = 8x8 screen pixels), 240x135 at 1080p. Coarser
// only makes the veto more permissive (a MAX texel covers more gaps), never wrong.
constexpr u32 kMaxSide = 256;
constexpr VkDeviceSize kSlotBytes = VkDeviceSize(kMaxSide) * kMaxSide * sizeof(float);
// A snapshot older than this (no pyramid built lately, a level load in between) is not used.
constexpr u32 kMaxAgeMs = 300;

struct Meta
{
    Fmatrix vp{};
    u32 w = 0, h = 0, mip = 0, w0 = 0, h0 = 0, time = 0;
    bool written = false;
};

struct Snap
{
    xr_vector<float> d;
    Meta m;
    bool valid = false;
};

CVulkanBuffer s_buf;
u8* s_mapped = nullptr;
Meta s_meta[kSlots];
u32 s_count = 0;

// Triple buffer: the render thread fills s_work and swaps it into s_ready; the main thread swaps s_ready into s_mine.
std::mutex s_mx;
Snap s_work, s_ready, s_mine;
bool s_readyNew = false;
} // namespace

void Capture(VkCommandBuffer cmd, const Fmatrix& viewProj, VkImage hzb, u32 w0, u32 h0, u32 mips)
{
    if (cmd == VK_NULL_HANDLE || hzb == VK_NULL_HANDLE || !w0 || !h0 || !mips)
        return;
    if (!s_mapped)
    {
        s_buf.Create(kSlots * kSlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO_PREFER_HOST, false, false,
                     true /*hostRead: cached, the CPU reads every texel*/);
        s_mapped = static_cast<u8*>(s_buf.Map());
        if (!s_mapped)
            return;
    }

    const u32 slot = s_count % kSlots;
    Meta& m = s_meta[slot];
    if (m.written)
    {
        // Copied kSlots frames ago: complete. Publish it before the copy below reuses the slot.
        s_buf.Invalidate();
        const float* src = reinterpret_cast<const float*>(s_mapped + slot * kSlotBytes);
        s_work.d.assign(src, src + size_t(m.w) * m.h);
        s_work.m = m;
        s_work.valid = true;
        std::lock_guard<std::mutex> lk(s_mx);
        std::swap(s_work, s_ready);
        s_readyNew = true;
    }
    ++s_count;

    u32 mip = 0;
    while (mip + 1 < mips && ((w0 >> mip) > kMaxSide || (h0 >> mip) > kMaxSide))
        ++mip;
    const u32 w = _max(1u, w0 >> mip), h = _max(1u, h0 >> mip);
    if (w > kMaxSide || h > kMaxSide)
    {
        m.written = false;
        return;
    }

    // The pyramid stays in GENERAL. Build's compute writes -> this copy; then the copy -> the next frame's build writes
    // (that build's own WAR barrier only waits on compute) and -> the host read kSlots frames later.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);

    VkBufferImageCopy region{};
    region.bufferOffset = VkDeviceSize(slot) * kSlotBytes;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, hzb, VK_IMAGE_LAYOUT_GENERAL, s_buf.GetHandle(), 1, &region);

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 0,
                         nullptr);
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);

    m.vp = viewProj;
    m.w = w; m.h = h; m.mip = mip; m.w0 = w0; m.h0 = h0;
    m.time = RV().dwTimeContinual;
    m.written = true;
}

void Destroy()
{
    s_buf.Destroy();
    s_mapped = nullptr;
    s_count = 0;
    for (Meta& m : s_meta) m = Meta{};
    std::lock_guard<std::mutex> lk(s_mx);
    s_work = Snap{}; s_ready = Snap{}; s_mine = Snap{};
    s_readyNew = false;
}

void BeginFrame()
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (s_readyNew)
    {
        std::swap(s_ready, s_mine);
        s_readyNew = false;
    }
}

bool BoxSeenOpen(const Fbox& box)
{
    const Snap& s = s_mine;
    if (!s.valid || s.d.empty() || Device.dwTimeContinual - s.m.time > kMaxAgeMs)
        return false;
    const Fmatrix& vp = s.m.vp;

    // Project the 8 corners with THAT frame's camera (same conventions as shaders/hzb_test.glsl: NDC z in [0,1],
    // depth cleared to 1, Y flipped by the negative-height viewport).
    float uMin = flt_max, uMax = -flt_max, vMin = flt_max, vMax = -flt_max, zMin = flt_max;
    for (int i = 0; i < 8; ++i)
    {
        Fvector p;
        box.getpoint(i, p);
        const float cw = p.x * vp._14 + p.y * vp._24 + p.z * vp._34 + vp._44;
        if (cw < 0.05f)
            return false;   // a corner at / behind the camera: unknown
        const float x = (p.x * vp._11 + p.y * vp._21 + p.z * vp._31 + vp._41) / cw;
        const float y = (p.x * vp._12 + p.y * vp._22 + p.z * vp._32 + vp._42) / cw;
        const float z = (p.x * vp._13 + p.y * vp._23 + p.z * vp._33 + vp._43) / cw;
        const float u = x * 0.5f + 0.5f, v = 0.5f - y * 0.5f;
        uMin = _min(uMin, u); uMax = _max(uMax, u);
        vMin = _min(vMin, v); vMax = _max(vMax, v);
        zMin = _min(zMin, z);
    }
    if (uMax < 0.f || uMin > 1.f || vMax < 0.f || vMin > 1.f || zMin <= 0.f)
        return false;   // not on that frame's screen: unknown

    // Footprint in mip-0 texels shifted into the copied mip (texel(mipK) == texel(mip0) >> K, see hzb_test.glsl), plus
    // one texel of margin for the jitter and the rounding -- a wider footprint can only find more open texels.
    const int w0 = int(s.m.w0), h0 = int(s.m.h0), sh = int(s.m.mip), w = int(s.m.w), h = int(s.m.h);
    auto tex = [](float f, int size, int shift) { return clampr(iFloor(f * float(size)), 0, size - 1) >> shift; };
    const int x0 = _max(tex(uMin, w0, sh) - 1, 0), x1 = _min(tex(uMax, w0, sh) + 1, w - 1);
    const int y0 = _max(tex(vMin, h0, sh) - 1, 0), y1 = _min(tex(vMax, h0, sh) + 1, h - 1);
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > 64 * 64)
        return true;   // covers a good part of the screen: not worth proving, keep it
    for (int y = y0; y <= y1; ++y)
    {
        const float* row = s.d.data() + size_t(y) * w;
        for (int x = x0; x <= x1; ++x)
            if (row[x] <= 0.f || row[x] >= zMin)   // 0 = never written (hzb_test.glsl treats it as visible too)
                return true;
    }
    return false;
}
} // namespace VK::HzbReadback
