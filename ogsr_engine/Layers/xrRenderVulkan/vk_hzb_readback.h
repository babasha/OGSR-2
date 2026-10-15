// vk_hzb_readback.h -- one coarse mip of the Hi-Z pyramid, read back to the CPU for NxOcc (CRender_Vulkan.cpp).
//
// WHY: r_dyn_occ 2 hides a skeleton when 5 rays to it all hit the cform. The cform has the planks of a fence where the
// screen has gaps between them, so an NPC seen through a fence was hidden (or flickered as the sample points drifted in
// and out of the gaps). The depth PREPASS draws alpha-tested geometry with its discard, so the pyramid (a MAX
// reduction of that depth) sees the gaps exactly: a box whose footprint has any texel farther than the box was not
// occluded in that frame. NxOcc uses it as a VETO only -- it never hides anything the rays would have kept -- so the
// frame lag (the ring below) cannot make an object pop in late.
//
// Render thread: Capture() right after the frame's final pyramid build. Main thread: BeginFrame() then BoxSeenOpen().
#pragma once

namespace VK::HzbReadback
{
// Render thread, outside a render pass, after the last BuildHZB of the frame: copy one mip into the ring and publish
// the copy made kSlots frames ago (its GPU work is complete by then: kSlots >= frames in flight).
// hzb: the pyramid (R32F, GENERAL), w0 x h0 its mip 0, mips its level count (CDetailManager::HZB*).
void Capture(VkCommandBuffer cmd, const Fmatrix& viewProj, VkImage hzb, u32 w0, u32 h0, u32 mips);
void Destroy();

// Main thread, once a frame before any BoxSeenOpen: take the newest published snapshot.
void BeginFrame();
// World-space box. true = in the snapshot some depth texel under the box's footprint is farther than the box's nearest
// point, i.e. the box was (at least partly) in view. false = occluded there, or unknown (no snapshot, too old,
// off its screen, camera too close) -- the caller then decides as it would without the readback.
bool BoxSeenOpen(const Fbox& box);
} // namespace VK::HzbReadback
