#version 450
#extension GL_GOOGLE_include_directive : require
// Cluster-LOD debug overlay (r_cluster_debug) — see vk_world_gpu DrawDebug.
// Redraws the culled indirect set; the cull shader stored the ENTRY ID in
// firstInstance, which Vulkan folds into gl_InstanceIndex (instanceCount=1) —
// a free per-cluster id with zero extra bindings or device features.
// Mode 4 (tint.a > 1.5) = LOD HEALTH view: reads the entry's parentError from
// the meta SSBO — red entries can NEVER be replaced by a coarser parent.
layout(location = 0) in vec3 aPos;
layout(location = 0) flat out uint vCluster;
layout(location = 1) flat out vec4 vHealth;

#include "cluster_meta.glsl"   // struct Meta — matches VK::WorldGPU::GpuMeshMeta
layout(set = 0, binding = 0) readonly buffer Metas { Meta metas[]; };

// tint: a > 1.5 = health view; 0.5..1.5 = mode 3 flat path color (per group).
layout(push_constant) uniform PC { mat4 mvp; vec4 tint; } pc;

void main()
{
    uint id  = uint(gl_InstanceIndex) & 0xFFFFFu;   // low 20 bits = entry id (high bits = crossfade fades)
    vCluster = id;
    if (pc.tint.a > 1.5) {
        float pe = metas[id].parentError;
        vHealth = pe >= 1e29 ? vec4(1.0, 0.1, 0.1, 1.0)    // INF: stalled/root — full detail forever
                : pe > 3.0   ? vec4(1.0, 0.55, 0.1, 1.0)   // replaced only hundreds of meters out
                : pe > 0.3   ? vec4(1.0, 1.0, 0.2, 1.0)    // moderate
                :              vec4(0.2, 1.0, 0.3, 1.0);   // healthy
    } else vHealth = vec4(0.0);
    gl_Position = pc.mvp * vec4(aPos, 1.0);
}
