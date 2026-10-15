// xrRenderVulkan - material NORMAL + GLOSS for the STATIC forward path
// (`<bump>.dds`, set 0 binding 4; loaded in vk_world_material.cpp).
//
// Statics had NEITHER in this renderer. Wall relief came only from the `#` height
// through the POM march, and the sky specular ran on ONE invented roughness for the
// whole world (world_lmap_frag_body: `roughI = 0.55` for plaster, rusted steel and
// glass alike), which is why r_ibl read as a uniform lacquer film over the frame
// instead of material-specific highlights. Both answers live in this one texture.
//
// R4/X-Ray packing, identical to what the terrain detail bumps already use in
// world_terrain.frag: normal = tex.wzy * 2 - 1, GLOSS = tex.x.
//
// #include AFTER light_ubo.glsl, FRAGMENT stages only (screen derivatives), and only
// in shaders that use the 5-binding STATIC material set — the terrain set has a
// different layout and must not see binding 4.
#ifndef BUMP_COMMON_GLSL
#define BUMP_COMMON_GLSL

#ifndef WORLD_BINDLESS   // the bindless twins: uTexBumpN = uWorldTex[5 * material + 4] (world_bindless.glsl)
layout(set = 0, binding = 4) uniform sampler2D uTexBumpN;   // .x = gloss, .wzy = tangent normal
#endif

// Cotangent frame from screen derivatives (Schueler). X-Ray statics carry no
// per-vertex tangents, so this is the only frame available — and it is the SAME one
// the POM march builds internally, so the normal map and the parallax agree.
mat3 bumpFrame(vec3 N, vec3 wp, vec2 uv)
{
    vec3 dp1 = dFdx(wp), dp2 = dFdy(wp);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    return mat3(T * inv, B * inv, N);
}

// Perturb `N` by the material normal map and return the material GLOSS in outGloss.
// `N` is the already-shading normal (POM-perturbed when POM ran) — the map REFINES
// it rather than replacing it, so a material with both keeps its parallax relief and
// gains its texture grain. Materials with no `<bump>` bind a 1x1 flat/zero-gloss
// fallback, so this is a no-op for them by construction (not by a branch).
vec3 bumpNormal(vec3 N, vec3 wp, vec2 uv, float strength, out float outGloss)
{
    vec4 s = texture(uTexBumpN, uv, L.spot_flash.z);   // DLSS mip bias (0 native)
    outGloss = s.x;
    if (strength <= 0.0) return N;
    // ⚠ RESIDENCY GUARD. A tangent-space normal ALWAYS points out of the surface, so
    // its z is positive — in this packing that is s.y > 0.5. A sample that fails the
    // test is not a normal at all: it is a texture tile the streamer has not delivered
    // yet, which reads (0,0,0,0) and decodes to (-1,-1,-1) — a perfectly well-formed
    // vector pointing INTO the surface, so a length check does not catch it and the
    // pixel shades pure black. That was the wandering black blocks on roofs and
    // fences: streaming pages, not a decode bug. Until the real data lands, shade
    // with the unperturbed normal.
    if (s.y <= 0.505) return N;
    vec3 nt = s.wzy * 2.0 - 1.0;
    nt.xy *= strength;
    if (dot(nt, nt) < 1e-8) return N;
    return normalize(bumpFrame(N, wp, uv) * normalize(nt));
}

#endif // BUMP_COMMON_GLSL
