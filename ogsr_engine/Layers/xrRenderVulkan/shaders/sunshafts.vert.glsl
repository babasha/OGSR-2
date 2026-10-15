#version 450
// xrRenderVulkan — SHARED fullscreen triangle (no vertex input, UV to location 0).
// ⚠ Keeps its historic name: the sun-shafts pass it was written for is gone
// (2026-07-24 — god rays are the froxel volumetrics' job now), but the light-cones
// pass loads THIS module by name (vk_pass_lightcones.cpp). Do not delete with the
// rest of the shafts files; rename only together with that Load() call.

layout(location = 0) out vec2 vUV;

void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
