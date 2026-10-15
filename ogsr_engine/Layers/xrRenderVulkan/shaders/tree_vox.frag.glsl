#version 450
// xrRenderVulkan — FORWARD CROWN CUBE TIER, fragment stage (r_tree_vox_dist).
//
// Deliberately almost empty. The tier exists because a cube is meant to be cheaper
// than the alpha-tested leaf cards it replaces, and that is only true if its
// FRAGMENT is cheap: a crown is hundreds of unsorted opaque boxes, so its fragment
// count runs several times its vertex count once overdraw is counted, and the cubes
// are not in the depth prepass (early-Z only helps within this pass). The whole
// shade — sky, probe field, sun and its VSM atlas walk, fog — therefore happens
// once per cube in tree_vox.vert and arrives here as one flat colour. Doing it per
// pixel instead made the tier cost MORE than the mesh it replaces (measured 24-08:
// Trees 8.0 -> 11.8 ms as the handover was pulled from off to 15 m).
//
// No alpha test and no texture fetch anywhere in the tier — that is the point.
layout(location = 0) in flat vec3  vColor;
layout(location = 1) in flat float vFade;
layout(location = 0) out vec4 outColor;

void main()
{
    // Dithered LOD crossfade (screen-door): the fine and coarse levels of a
    // transitioning tree draw with COMPLEMENTARY masks of the same per-pixel noise —
    // fade > 0 keeps a pixel where noise >= fade (dissolving out), fade < 0 keeps
    // noise < |fade| (dissolving in), so exactly one level survives per pixel and
    // coverage never drops mid-fade.
    if (vFade != 0.0) {
        float ign = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        if (vFade > 0.0) { if (ign <  vFade) discard; }
        else             { if (ign >= -vFade) discard; }
    }
    outColor = vec4(vColor, 1.0);
}
