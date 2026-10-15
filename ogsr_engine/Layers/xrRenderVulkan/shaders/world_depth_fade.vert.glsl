#version 450
// Depth-prepass variant for the cluster-LOD crossfade (r_cluster_fade): same
// minimal position-only transform as shadow_depth.vert, plus the cull-packed
// fade bits (firstInstance → gl_InstanceIndex) for the dithering fragment.
// The prepass MUST dither identically to the color pass — the color depth test
// is LEQUAL against this depth, so mismatched coverage = holes at transitions.
layout(location = 0) in vec3 aPos;
layout(location = 0) flat out uint vFadeBits;

layout(push_constant) uniform PC { mat4 mvp; } pc;

void main()
{
    vFadeBits   = uint(gl_InstanceIndex);
    gl_Position = pc.mvp * vec4(aPos, 1.0);
}
