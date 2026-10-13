#version 450

// xrRenderVulkan — Stage-1 Volumetric Media System: SMOKE MEDIA RESOLVE.
//
// Converts the smoke accumulation SSBO (atomic fixed-point, written by vol_splat)
// into a filtered RGBA16F media volume vol_inject samples:
//   rgb = density-weighted average albedo (sum(d*color)/sum(d))
//   a   = density (extinction the smoke adds to the medium)
// One thread per smoke-grid cell. Same coarse grid as the splat; vol_inject reads
// it trilinearly at the matching normalized frustum coords.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 0) readonly buffer Accum { uint accum[]; };
layout(set = 0, binding = 1, rgba16f) uniform writeonly image3D uMedia;

layout(push_constant) uniform Push {
    ivec4 dims;      // xyz = smoke grid dims
    vec4  params;    // x = 1/fixedScale
} pc;

void main()
{
    ivec3 id = ivec3(gl_GlobalInvocationID);
    if (any(greaterThanEqual(id, pc.dims.xyz))) return;

    uint  cell = (uint(id.z) * uint(pc.dims.y) + uint(id.y)) * uint(pc.dims.x) + uint(id.x);
    float inv  = pc.params.x;
    float d    = float(accum[cell * 4u + 0u]) * inv;
    vec3  cw   = vec3(accum[cell * 4u + 1u], accum[cell * 4u + 2u], accum[cell * 4u + 3u]) * inv;
    vec3  albedo = (d > 1e-4) ? cw / d : vec3(0.0);
    imageStore(uMedia, id, vec4(albedo, d));
}
