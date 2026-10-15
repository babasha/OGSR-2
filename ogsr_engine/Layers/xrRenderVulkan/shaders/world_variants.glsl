// xrRenderVulkan - world uber-shader variant specialization constants (Inc 1).
//
// The world FS (world_lmap/vlit/terrain) was a monolithic uber-shader whose VGPR
// budget was sized for the worst path (POM march + snow + wet + IBL + debug), so
// even a flat, dry, summer pixel paid the register pressure of features it skipped
// → low occupancy → the GPU couldn't hide texture latency.
//
// These booleans are Vulkan specialization constants: the CPU (vk_pipeline_cache
// CreatePipeline) bakes concrete values per (material, frame) at pipeline-creation
// time, and the driver dead-code-eliminates the false branches INCLUDING their
// registers. Defaults are all-true so an unspecialized pipeline behaves exactly like
// the old monolithic shader (safe fallback).
//
// Bit layout MUST match WorldSpec* in vk_pipeline_cache.h:
//   bit0 POM   bit1 SNOW   bit2 WET   bit3 IBL   bit4 DEBUG   bit5 FEEDBACK
//   bit6 DDGI  (constant_id 7 = RR guides, keyed separately — see below)
#ifndef WORLD_VARIANTS_GLSL
#define WORLD_VARIANTS_GLSL

layout(constant_id = 0) const bool SPEC_POM   = true;   // material has a real `#` height (mat->tessellated)
layout(constant_id = 1) const bool SPEC_SNOW  = true;   // snow accumulation active this frame (sf_params.w > 0)
layout(constant_id = 2) const bool SPEC_WET   = true;   // rain/wetness active this frame (rain_params.y > 0)
layout(constant_id = 3) const bool SPEC_IBL   = true;   // sky specular IBL active this frame (r_ibl)
layout(constant_id = 4) const bool SPEC_DEBUG = true;   // any r_*_debug view active (0 in normal play)
// Texture-streaming feedback (txfbReport atomicMin) master gate: false (streamer
// off) DCEs the atomic away entirely. NOTE the atomic is an FS SIDE EFFECT, which
// forbids automatic early-Z (measured 6-8x FS invocations vs visible samples,
// Кордон 23-07-2026); the occluded-fragment cost is solved by the EARLY_ZTEST
// shader twin on no-z-write statics pipelines, NOT by toggling this bit.
layout(constant_id = 5) const bool SPEC_FEEDBACK = true;
// DDGI probe field in the ambient term (r_ddgi_light). False compiles the whole
// lookup away — 16 atlas taps and the eight-probe loop with them — so the volume
// costs a surface exactly nothing while it is off, which is also what keeps the
// off path bit-identical to the shader that existed before it.
layout(constant_id = 6) const bool SPEC_DDGI = true;
// DLSS Ray Reconstruction guide writes (r_dlss_rr). Unlike every other bit here
// this one is NOT part of the WorldSpec mask: it is stamped straight from
// PipelineCache::Key::rrGuides, because it must track the ATTACHMENT LIST
// exactly. False DCEs the EnvBRDFApprox2 evaluation and the three stores — which
// matters, because the guide outputs are declared unconditionally (writes to a
// location with no attachment are discarded) and the world FS is the hottest
// shader in the renderer; "discarded" must not mean "computed anyway".
layout(constant_id = 7) const bool SPEC_RRGUIDE = true;
// A lamp in this frame has a shadow map (spot tile / point cube) or the flashlight
// cookie (vk_env_light: WS_LAMPSH). Off compiles the per-lamp shadow lookups out of
// shadeDynLight -- on GM20B they alone took the R1 terrain from 40 to 64 registers.
layout(constant_id = 8) const bool SPEC_LAMPSH = true;

#endif
