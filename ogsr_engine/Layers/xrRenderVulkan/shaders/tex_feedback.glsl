// Texture-streaming GPU feedback ("sampler feedback lite", UE-VT style).
// Set 1 binding 30 — device-local u32[TXFB_SLOTS] owned by vk_texture_stream.
// The frame refills it with 0xFFFFFFFF ("not sampled"); world fragment shaders
// atomicMin the encoded LOD they actually wanted for their base diffuse. The CPU
// reads it back (3-frame latency) and streams mips from FACT of visibility.
//
// Encode must mirror TextureStreamer's DecodeFbLod: enc = (lod + 16) * 4, so a
// NEGATIVE lod (magnification = "I want sharper mips than are resident") stays
// representable. lod is textureQueryLod().y — unclamped, relative to the image's
// resident base mip — plus the DLSS mip bias the color sample used.

#ifndef TXFB_SLOTS
#define TXFB_SLOTS 8192u
#endif

layout(set = 1, binding = 30) buffer TexFeedbackSSBO { uint uTxFeedback[]; };

// Report from 1 of every 4x4 pixels — a per-texture MIN needs coverage, not
// density, and this keeps the atomic traffic ~6% of full rate.
void txfbReport(uint id, float lod)
{
    if (id >= TXFB_SLOTS) return;                      // 0xFFFFFFFF = not streamable
    if (((uint(gl_FragCoord.x) | uint(gl_FragCoord.y)) & 3u) != 0u) return;
    uint enc = uint(clamp((lod + 16.0) * 4.0, 0.0, 4095.0));
    atomicMin(uTxFeedback[id], enc);
}
