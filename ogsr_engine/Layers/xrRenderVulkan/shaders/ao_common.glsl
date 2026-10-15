// xrRenderVulkan — ambient-occlusion helpers with no descriptor dependencies.
//
// Pure functions of their arguments, so every receiver can share them regardless
// of which descriptor set (or which stage) it binds: the opaque world reaches them
// through env_common.glsl, the foliage passes (detail/tree) include this directly
// because they cannot pull env_common in.
#ifndef AO_COMMON_GLSL
#define AO_COMMON_GLSL

// Colored AO (R4 / Activision SIGGRAPH'16): mid-range occlusion bends toward the
// albedo colour. Full open (ao=1) and full black (ao=0) are unchanged.
vec3 coloredAO(float ao, vec3 albedo)
{
    vec3 a =  2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c =  2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}

#endif // AO_COMMON_GLSL
