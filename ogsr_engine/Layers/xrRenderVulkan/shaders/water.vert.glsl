#version 450
// xrRenderVulkan — WATER surface, vertex stage.
//
// Level water is ordinary static geometry carrying the level shader
// `effects\water` (see CVulkanShader::Create). Before this pass existed it was
// classified as nothing at all and rode the opaque vert-lit path, where the
// baked vertex light and the baked sky access of a water polygon are ~0 — the
// albedo got multiplied by zero and every pond in the game rendered BLACK.
//
// Same stride-32 world vertex as world_vlit/world_lmap: position at 0, packed
// normal (D3DCOLOR BGRA) at 12, base UV (SHORT2 sscaled) at 24 (lmap
// sub-layout) or 28 (vert-lit) — the pipeline's attribute offset picks the
// right one, so one shader serves both.
//
// This stage is deliberately a pass-through. When tessellation is available the
// TESE owns the final position (it displaces by the wave field); when it is not,
// gl_Position written here is the final one. Both paths share this shader.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inNormal;     // D3DCOLOR @ 12 (BGRA in memory)
layout(location = 2) in vec2 inUV_short;   // SHORT2 SSCALED (uv * 1024)

layout(push_constant) uniform PC {
    mat4 mvp;    // world -> clip (level water is identity-model, so mvp == viewProj)
    vec4 basin;  // per-surface fetch; unused here, declared so the block matches
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main()
{
    // Level statics submit an identity model matrix (Pass_Water skips anything
    // else), so the raw position IS the world position — same assumption the
    // world VS makes for statics.
    vWorldPos   = inPos;
    vNormal     = normalize(inNormal.bgr * 2.0 - 1.0);   // D3DCOLOR BGRA -> xyz
    vUV         = inUV_short * (1.0 / 1024.0);
    gl_Position = pc.mvp * vec4(inPos, 1.0);
}
