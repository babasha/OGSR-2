// xrRenderVulkan — the screen-space noise of the R1 tree crossfade (nfs23, r_tree_lod_band).
//
// Over its group's band the FLOD billboard keeps the pixels with lodDither() < its opacity
// (lod_imposter.frag) and the crown mesh keeps the pixels with lodDither() >= the SAME value
// (tree.frag / tree_r1.frag, vLodFade from tree.vert) -- complementary masks, so each pixel
// shows the billboard or the crown, never both on top of each other and never neither.
// ONE function for both sides on purpose: two copies drifting apart would bring the overlap back.
#ifndef LOD_DITHER_GLSL
#define LOD_DITHER_GLSL

// Interleaved gradient noise (Jimenez 2014) on the fragment's pixel position.
float lodDither()
{
    return fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
}

#endif // LOD_DITHER_GLSL
