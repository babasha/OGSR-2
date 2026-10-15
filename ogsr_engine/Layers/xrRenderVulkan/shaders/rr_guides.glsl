// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// ============================================================================
//  DLSS RAY RECONSTRUCTION — GUIDE WRITES (shared by every opaque forward shader)
// ============================================================================
//  The C++ side is vk_rr_guides.{h,cpp}; this is the half that fills the targets.
//
//  ⚠ THE OUTPUTS ARE DECLARED UNCONDITIONALLY. Vulkan discards writes to a
//  fragment output whose location has no colour attachment, so a shader that
//  includes this stays legal in a one-attachment scope (r_dlss_rr 0) — the
//  pipeline's attachment list is what changes, keyed by PipelineCache::Key::
//  rrGuides. That is what buys us a live toggle with ONE compiled shader
//  instead of a with/without SPIR-V variant of every world shader.
//
//  ⚠⚠ ROUGHNESS IS LINEAR AND LIVES IN normal.w. The feature is created with
//  NVSDK_NGX_DLSS_Roughness_Mode_Packed; handing RR a separate roughness texture
//  as well would give it two sources that can disagree.
// ============================================================================
#ifndef RR_GUIDES_GLSL
#define RR_GUIDES_GLSL

layout(location = 1) out vec4 outRRNormalRough;   // world-space shading normal .xyz + linear roughness .w
layout(location = 2) out vec4 outRRDiffAlbedo;    // diffuse reflectance (LINEAR, never sRGB)
layout(location = 3) out vec4 outRRSpecAlbedo;    // EnvBRDFApprox2(F0, rough², NdotV)

// Specular albedo = the average specular reflectivity for THIS view direction.
// Verbatim from the DLSS-RR Integration Guide's appendix (itself Ray Tracing Gems
// ch. 32) — do not "simplify" it: RR was trained against these coefficients, and a
// different Fresnel approximation is a different guide, not a cheaper one.
vec3 rrEnvBRDFApprox2(vec3 specularColor, float alpha, float NoV)
{
    NoV = abs(NoV);
    vec4 X;
    X.x = 1.0;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;
    vec4 Y;
    Y.x = 1.0;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;

    // ⚠GLSL mat2/mat3 constructors are COLUMN-major while HLSL float2x2/float3x3
    // literals are ROW-major, so every matrix here is the transpose of the SDK's
    // listing. mul(M, v) in HLSL is then M * v with these transposed matrices.
    const mat2 M1 = mat2(0.99044,  1.29678,
                        -1.28514, -0.755907);
    const mat3 M2 = mat3(1.0,      20.3225,  121.563,
                         2.92338, -27.0302,  626.13,
                         59.4188,  222.592,  316.627);
    const mat2 M3 = mat2(0.0365463, 9.0632,
                         3.32707,  -9.04756);
    const mat3 M4 = mat3(1.0,       9.04401,   5.56589,
                         3.59685, -16.3174,   19.7886,
                        -1.36772,   9.22949, -20.2123);

    float bias  = dot(M1 * X.xy,  Y.xy)  / dot(M2 * X.xyw, Y.xyw);
    float scale = dot(M3 * X.xy,  Y.xy)  / dot(M4 * X.xzw, Y.xyw);

    // The SDK's own words: "This is a hack for specular reflectance of 0".
    bias *= clamp(specularColor.g * 50.0, 0.0, 1.0);
    return specularColor * max(0.0, scale) + max(0.0, bias);
}

// N  — world-space SHADING normal (after POM/bump/snow — the vector the lighting used)
// rough — LINEAR roughness in [0,1]
// diffAlbedo — the colour that multiplied the diffuse lighting
// F0 — specular reflectance at normal incidence (0.04 for our dielectrics)
// V  — unit vector from the surface TOWARD the eye
void rrWriteGuides(vec3 N, float rough, vec3 diffAlbedo, vec3 F0, vec3 V)
{
    outRRNormalRough = vec4(normalize(N), clamp(rough, 0.0, 1.0));
    outRRDiffAlbedo  = vec4(max(diffAlbedo, vec3(0.0)), 1.0);
    outRRSpecAlbedo  = vec4(rrEnvBRDFApprox2(F0, rough * rough, dot(N, V)), 1.0);
}

#endif  // RR_GUIDES_GLSL
