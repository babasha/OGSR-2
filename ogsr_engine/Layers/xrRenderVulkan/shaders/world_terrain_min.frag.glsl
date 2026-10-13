#version 450
// nx_statab 34 (Switch, v143): the terrain colour draw with the SMALLEST possible fragment shader -- no inputs, no
// descriptors, one constant. Read against "terrain const colour" (18: the real terrain FS returning at its first line):
// the difference is what merely LAUNCHING the real terrain FS costs per fragment (registers / occupancy, interpolant
// setup, anything the compiler hoisted above that early return).
//
// v143 answered: the 1-line FS takes back 65-90 % of the terrain's ~8 ms, const colour 0, colour write 0, depth test 0
// -- the price is the real FS's PROGRAM, not what it executes. v144 adds one property of the real FS at a time:
//   -DMIN_INPUTS (world_terrain_min_in.frag.spv,   nx_statab 35): + the real FS's 5 interpolated inputs
//   -DMIN_KILL   (world_terrain_min_kill.frag.spv, nx_statab 36): + a discard (never taken; the program can kill)
#ifdef MIN_INPUTS
layout(location = 0) in  vec2 vUV;
layout(location = 1) in  vec2 vDetailUV;
layout(location = 2) in  vec2 vLmapUV;
layout(location = 3) in  vec3 vWorldPos;
layout(location = 4) in  vec3 vNormal;
#endif
layout(location = 0) out vec4 outColor;

void main()
{
#ifdef MIN_KILL
    if (gl_FragCoord.z < 0.0) discard;   // never true, but the compiler cannot know: the shader is marked as killing
#endif
    outColor = vec4(0.22, 0.24, 0.16, 1.0);
#ifdef MIN_INPUTS
    // Every component interpolated; the 1e-7 keeps the colour and stops the compiler dropping the reads.
    outColor.rgb += 1e-7 * (vec3(vUV + vDetailUV + vLmapUV, 0.0) + vWorldPos + vNormal);
#endif
}
