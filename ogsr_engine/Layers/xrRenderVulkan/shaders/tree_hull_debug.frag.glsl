#version 450
// xrRenderVulkan — crown-HULL debug overlay FS ("voxels instead of leaves", UE-style
// viewmode). FLAT face-shading: the face normal comes from screen-space derivatives of
// the world position (no per-vertex normals needed), so each voxel face / hull facet
// reads as a distinct lit surface — you SEE the voxel cubes, not a pink blob. Warm sun
// N·L + cool sky ambient by facing, near-opaque so the proxy replaces the crown visually.
layout(location = 0) in vec3 vWorld;
layout(location = 0) out vec4 oColor;

void main()
{
    vec3 N = normalize(cross(dFdx(vWorld), dFdy(vWorld)));
    // Sun roughly overhead-ish; two-sided (CULL_NONE hull) so back faces still shade.
    vec3 L = normalize(vec3(0.35, 0.9, 0.2));
    float nl = abs(dot(N, L));
    // Hemispheric tint: up-facing warm, down-facing cool — makes cube tops/sides differ.
    float up = 0.5 + 0.5 * N.y;
    vec3 warm = vec3(0.55, 0.85, 0.35);   // lit foliage-green voxel
    vec3 cool = vec3(0.12, 0.20, 0.14);   // shadowed underside
    vec3 c = mix(cool, warm, nl * 0.75 + up * 0.25);
    oColor = vec4(c, 0.92);
}
