#version 450
#extension GL_GOOGLE_include_directive : require

// Fullscreen triangle for the sky pass — no VBO, vertices generated from
// gl_VertexIndex. Three verts at clip-space (-1,-1) / (3,-1) / (-1,3) cover
// the screen with one triangle. z = 1 puts every fragment at the far plane
// so the depth test (LESS_OR_EQUAL) only passes where the world hasn't drawn.
//
// World direction is built from camera basis vectors (right/up/forward) and
// per-pixel screen offset. This is purely orientation-dependent — the sky is
// at infinity, so camera position must not influence it. (Earlier this used
// invViewProj * far/near subtraction; that's mathematically position-
// invariant only with infinite precision and was mixing translation into the
// per-frame direction in practice.)

// Manual vec4 packing — push_constant default layout is implementation-
// defined for vec3+scalar mixes; vec4s eliminate that ambiguity.
// Full push block must match sky.frag.glsl exactly (shared range across stages).
// The VS only reads the first three vec4s; the rest are consumed by the FS.
// Was a push block; moved to a UBO when it outgrew the 128-byte guaranteed push
// limit (see sky_ubo.glsl). The VS reads only the camera basis.
#include "sky_ubo.glsl"

layout(location = 0) out vec3 vWorldDir;

void main()
{
    vec2 pos = vec2(
        (gl_VertexIndex == 1) ?  3.0 : -1.0,
        (gl_VertexIndex == 2) ?  3.0 : -1.0
    );
    gl_Position = vec4(pos, 1.0, 1.0);

    // pos.x ∈ [-1, 1] across the screen width, pos.y ∈ [-1, 1] across height.
    // Viewport is flipped (vp.height < 0) so pos.y = +1 corresponds to top of
    // screen — matches X-Ray's vCameraTop pointing world-up.
    vWorldDir = S.camForward_alt.xyz
              + pos.x * S.camRightTan_rot.xyz
              + pos.y * S.camUpTan_weight.xyz;
}
