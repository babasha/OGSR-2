// xrRenderVulkan - bindless materials for the GPU-driven world statics (r_world_bindless, 10-09).
//
// The GPU world used to draw one indirect group per MATERIAL (636 on Cordon, 482 materials): every group a
// descriptor-set bind + push + vkCmdDrawIndexedIndirectCount. On the Switch (NVK on Maxwell) each of those is
// several pushbuffer segments the command processor fetches one by one while the 3D engine waits -- the colour
// pass of the statics cost 0.80 of its 2.10 timestamp units with EMPTY commands (v288 r_world_empty_probe).
// The opaque statics now draw ~32 groups keyed by vertex/index buffer only; the material travels per draw:
//   firstInstance (low 20 bits = cull entry id) -> worldEntryMat[] -> material index -> uWorldTex[5 * mat + k].
// Compiled into the *_bl twins of the world lmap/vlit shaders (-DWORLD_BINDLESS); set 0 = this set instead of the
// per-material one. Slots per material: 0 base, 1 detail, 2 lightmap, 3 bump (x), 4 bump normal + gloss.
#ifndef WORLD_BINDLESS_GLSL
#define WORLD_BINDLESS_GLSL
#ifdef WORLD_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require

struct WorldMatRec { float detailScale; uint streamID; uint _p0; uint _p1; };
layout(std430, set = 0, binding = 1) readonly buffer WorldMatRecs  { WorldMatRec worldMatRec[]; };

#ifdef WORLD_BINDLESS_VS
layout(std430, set = 0, binding = 2) readonly buffer WorldEntryMats { uint worldEntryMat[]; };
layout(location = 8) flat out uint vMatId;
#define WORLD_DETAIL_SCALE worldMatRec[vMatId].detailScale
#else
layout(set = 0, binding = 0) uniform sampler2D uWorldTex[];
layout(location = 8) flat in uint vMatId;
#define WORLD_TEX(k)       uWorldTex[nonuniformEXT(vMatId * 5u + (k))]
#define uTexDiffuse        WORLD_TEX(0u)
#define uTexDetail         WORLD_TEX(1u)
#define uTexLmap           WORLD_TEX(2u)
#define uTexBumpX          WORLD_TEX(3u)
#define uTexBumpN          WORLD_TEX(4u)
#define WORLD_DETAIL_SCALE worldMatRec[vMatId].detailScale
#define WORLD_STREAM_ID    worldMatRec[vMatId].streamID
#endif

#endif // WORLD_BINDLESS
#endif // WORLD_BINDLESS_GLSL
