#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — GPU per-light grass caster cull with COMPACTION (Phase 1 of
// the "grass × dynamic lights" plan). One thread per grass caster instance slot
// in the detail manager's GPU-driven buffer (VisibleSSBO — the CASTER set, 1
// frame stale, same convention as vsm_grass_bin). Each thread loops over this
// frame's shadow lights (≤16 spheres: spot beams + campfire cubes) and, for every
// light whose sphere the blade's base falls inside, tallies it into that
// (light,type) cell. This is the COUNT half of a counting-sort compaction:
//   pass 0 (count):   atomicAdd(cellCount[light*T + type])
//   [setup]           prefix-sums the counts → per-cell arena base + indirect cmd
//   pass 1 (scatter): copies the 64 B instance into arena[base + slot]
// The shadow depth passes then draw the compact per-cell region via indirect
// (firstInstance = base), turning a full VS pass over tens of thousands of blades
// per light/face into a few hundred. See vk_pass_shadow.cpp (GrassCull).
layout(local_size_x = 64) in;

struct DetailInstance { vec4 row0, row1, row2, color; };   // 64 B (translation = row*.w)

layout(set = 0, binding = 0) readonly buffer Visible  { DetailInstance inst[]; };
layout(set = 0, binding = 1) readonly buffer Indirect { uint indirect[]; };        // detail: 5 u32/type, instanceCount @ +1
layout(set = 0, binding = 2) uniform  Lights          { vec4 lightPosRange[16]; } L; // xyz = pos, w = cull radius (range+margin)
layout(set = 0, binding = 3) buffer Count             { uint cellCount[]; };
layout(set = 0, binding = 4) buffer Cursor            { uint cellCursor[]; };        // scatter start = base (set by setup)
layout(set = 0, binding = 5) writeonly buffer Arena   { DetailInstance arena[]; };

layout(push_constant) uniform PC {
    uint sectionSize;   // per-type stride in the detail buffer (instances)
    uint typeCount;     // grass types (== cell stride)
    uint numLights;
    uint pass;          // 0 = count, 1 = scatter
    uint maxCull;       // arena capacity (instances) — overflow guard
    uint pad0, pad1, pad2;
} pc;

void main()
{
    uint g = gl_GlobalInvocationID.x;
    if (g >= pc.sectionSize * pc.typeCount) return;

    uint type  = g / pc.sectionSize;
    uint local = g - type * pc.sectionSize;
    uint count = indirect[type * 5u + 1u];        // stale instanceCount for this type
    if (local >= count) return;                    // padding slot — no instance

    DetailInstance di = inst[g];
    vec3 c = vec3(di.row0.w, di.row1.w, di.row2.w);

    for (uint l = 0u; l < pc.numLights; ++l) {
        vec3  d = c - L.lightPosRange[l].xyz;
        float r = L.lightPosRange[l].w;
        if (dot(d, d) > r * r) continue;           // outside this light's sphere
        uint cell = l * pc.typeCount + type;
        if (pc.pass == 0u) {
            atomicAdd(cellCount[cell], 1u);
        } else {
            uint dst = atomicAdd(cellCursor[cell], 1u);
            if (dst < pc.maxCull) arena[dst] = di;  // packed by setup's prefix sum
        }
    }
}
