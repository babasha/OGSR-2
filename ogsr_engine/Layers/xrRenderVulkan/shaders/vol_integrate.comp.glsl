#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — froxel volumetric INTEGRATION (vk_volumetrics, P1).
//
// One thread per (x,y) froxel column. March front-to-back over Z, accumulating
// in-scatter and transmittance (Beer-Lambert). Each froxel stores the running
// accumulation up to its slice so the composite (folded into the tonemap) can
// sample at the scene pixel's depth: scene = scene*transmittance + inscatter.
//
//   T          = product of exp(-extinction * sliceLen)  (camera → this slice)
//   accum.rgb += sliceInScatter * T_before * sliceLen
//
// The slice world thickness comes from the SAME exp-Z mapping the inject used
// (near, log2(far/near)) so geometry depth ↔ froxel Z stays consistent.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "vol_params.glsl"   // Vol UBO (set 0 b0) — matches VK::Volumetrics::VolUBO

layout(set = 0, binding = 1, rgba16f) uniform readonly  image3D uScatter;     // rgb=in-scatter, a=extinction
layout(set = 0, binding = 2, rgba16f) uniform writeonly image3D uIntegrated;  // rgb=accum, a=transmittance

layout(push_constant) uniform PC {
    vec4 p;   // x = r_vol_hillaire (0 = legacy front-face accumulation), yzw reserved
} pc;

void main()
{
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    int dimX = int(V.gridParams.x), dimY = int(V.gridParams.y), dimZ = int(V.gridParams.z);
    if (px.x >= dimX || px.y >= dimY) return;

    float nearZ = V.zParams.x, logFN = V.zParams.z;

    vec3  accum = vec3(0.0);
    float T     = 1.0;
    float invDimZ = 1.0 / float(dimZ);

    for (int z = 0; z < dimZ; ++z) {
        float zPrev = nearZ * exp2(logFN * float(z)       * invDimZ);
        float zNext = nearZ * exp2(logFN * float(z + 1)   * invDimZ);
        float sliceLen = zNext - zPrev;

        vec4  s   = imageLoad(uScatter, ivec3(px, z));
        float ext = max(s.a, 0.0);

        // V-0 — ENERGY-CONSERVING SLICE INTEGRAL (Hillaire, Frostbite 2015).
        //
        // The legacy line was `accum += s.rgb * T * sliceLen`: the slice's whole
        // in-scatter attenuated by the transmittance at its FRONT FACE. Light born at
        // the back of a slice therefore travelled the slice for free, and the error
        // grows with density — so thickening the fog DARKENED the scene faster than it
        // lit it (the "dirty cigarette smoke" look), brightness was non-linear in
        // density, and Z showed banding wherever slices are thick.
        //
        // The closed form integrates in-scatter ALONG the slice against its own
        // attenuation: ∫₀ᵈ S·exp(-σt)dt = (S - S·exp(-σd)) / σ. As σ→0 this tends to
        // S·d, i.e. it degrades exactly into the old formula for thin air — the
        // difference shows up precisely where the old one was wrong.
        float sliceT = exp(-ext * sliceLen);
        if (pc.p.x > 0.5) {
            // ⚠ The closed form has a REMOVABLE SINGULARITY at sigma = 0: numerator and
            // denominator both vanish, and `/max(ext,1e-6)` does not rescue it — it
            // computes 0/1e-6 = 0, i.e. air with no extinction emits NOTHING. That is
            // silently wrong for any emissive-but-transparent volume, and it blacked out
            // every debug view that pins extinction to 0 to read "at full range"
            // (r_vol_debug 3/4 and the ground forensics) from the day V-0 landed.
            // Take the analytic limit S*d there instead — which is also exactly the
            // legacy formula, so thin air is continuous across the branch.
            vec3 sInt = (ext > 1e-4) ? (s.rgb - s.rgb * sliceT) / ext
                                     : s.rgb * sliceLen;
            accum += sInt * T;
        } else {
            accum += s.rgb * T * sliceLen;   // legacy A/B (r_vol_hillaire 0)
        }
        T *= sliceT;

        imageStore(uIntegrated, ivec3(px, z), vec4(accum, T));
    }
}
