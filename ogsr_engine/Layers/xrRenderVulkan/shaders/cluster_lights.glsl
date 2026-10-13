// xrRenderVulkan — clustered forward receiver side (vk_clustered).
//
// Declares the three cluster SSBOs (light list / per-cluster count / index list)
// and the fragment->cluster lookup. Include AFTER the Lighting UBO `L` is
// declared (the loops below read L.cluster_params / eye_pos / cam_dir / ao_params).
// Define CLUSTER_SET before including: 1 for the world pass, 2 for skinned/trees.
//
// Layout MUST match light_cluster.comp.glsl (std430): ClusterLight == that
// shader's Light (3 vec4); clusterGrid[ci] == count; clusterIndex[ci*MAXPER+k]
// == a light index into clusterLights[]. The cluster index packing matches the
// compute: ci = (cz*GY + cy)*GX + cx.

#ifndef CLUSTER_SET
#define CLUSTER_SET 1
#endif

struct ClusterLight { vec4 pos; vec4 color; vec4 dir; };
layout(std430, set = CLUSTER_SET, binding = 17) readonly buffer ClusterLights { ClusterLight clusterLights[]; };
layout(std430, set = CLUSTER_SET, binding = 18) readonly buffer ClusterGrid   { uint clusterGrid[];   };
layout(std430, set = CLUSTER_SET, binding = 19) readonly buffer ClusterIndex  { uint clusterIndex[];  };

// Base cluster index for this fragment. fragXY = gl_FragCoord.xy, invScreen =
// (1/W, 1/H) (= L.ao_params.xy). cp = cluster_params (sliceScale, sliceBias,
// near, enable); cp2 = cluster_params2 (GX, GY, GZ, MAXPER).
int clusterOfFragment(vec3 wp, vec3 eye, vec3 camDir, vec2 fragXY, vec2 invScreen, vec4 cp, vec4 cp2)
{
    float zv = max(dot(wp - eye, camDir), cp.z);            // view-space depth (>= near)
    int GX = int(cp2.x), GY = int(cp2.y), GZ = int(cp2.z);
    int slice = int(clamp(floor(log2(zv) * cp.x + cp.y), 0.0, float(GZ - 1)));
    int tx = int(clamp(fragXY.x * invScreen.x * cp2.x, 0.0, cp2.x - 1.0));
    int ty = int(clamp(fragXY.y * invScreen.y * cp2.y, 0.0, cp2.y - 1.0));
    return (slice * GY + ty) * GX + tx;
}

// Light count in this fragment's cluster — for the r_clustered_debug heatmap.
int clusterLightCount(vec3 wp, vec3 eye, vec3 camDir, vec2 fragXY, vec2 invScreen, vec4 cp, vec4 cp2)
{
    return int(clusterGrid[clusterOfFragment(wp, eye, camDir, fragXY, invScreen, cp, cp2)]);
}

// Debug heatmap ramp. EMPTY clusters are BLACK (so "is there any light here?"
// reads unambiguously); 1 light = blue, ~3 = green/yellow, 6+ = red.
vec3 clusterHeat(int count)
{
    if (count <= 0) return vec3(0.0);                      // empty = black
    float t = clamp(float(count - 1) / 5.0, 0.0, 1.0);     // 1 → blue … 6+ → red
    return clamp(vec3(2.0 * t, 1.0 - abs(2.0 * t - 1.0), 1.0 - 2.0 * t), 0.0, 1.0);
}
