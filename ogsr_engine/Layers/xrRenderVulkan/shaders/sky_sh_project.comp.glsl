#version 450
// xrRenderVulkan - project the world-space sky probe onto 9 spherical harmonics.
//
// WHY: diffuse sky ambient used to be ONE texel of the sky cube fetched along the
// surface normal at LOD 0. That is not irradiance — it is a point sample of a
// sharp skybox. Three things went wrong at once:
//   * it could not be blurred (the weather cubes ship BC-compressed, single-mip),
//   * so feeding it a per-pixel detail normal would have aliased, which is why
//     terrain fed it the flat geometric normal instead and lost all relief, and
//   * an up-facing normal lands on the +Y face, the coolest, flattest part of a
//     sunset sky, while the warm horizon band that carries almost all of the
//     energy at dusk sits at the BOTTOM edge and is unreachable.
// Net result at sunset: a beautifully graded sky over uniformly-lit ground.
//
// SH9 is the standard fix (Ramamoorthi & Hanrahan 2001): irradiance from a
// distant environment is so low-frequency that 9 coefficients capture it to ~1%
// error. Once projected, evaluating it per pixel is a handful of MADs, it is
// smooth BY CONSTRUCTION (so the detail normal can finally be used without
// aliasing), and it is directional in azimuth for free — the sunset side of the
// sphere genuinely carries more energy, so slopes facing the sunset warm up.
//
// Source is the PREFILTERED cube (vk_ibl), which is already unwrapped into world
// space (half-cube remap + sky_rotation applied, see sky_halfcube.glsl). Feeding
// this the raw weather cube instead would project the AUTHORING space and bake in
// exactly the horizon misweighting we are here to remove. A low mip is plenty:
// we are integrating the sphere down to 9 numbers, so high-frequency detail is
// discarded anyway.
//
// One workgroup, 64 threads, strided over 6*N*N texels, then a shared-memory
// reduction. The whole dispatch is a few thousand texel fetches and runs only
// when the weather cubes or the cross-fade move (vk_ibl.cpp change detection).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform samplerCube uProbe;   // world-space sky probe (vk_ibl)

layout(std430, set = 0, binding = 1) buffer SkySH {
    vec4 c[9];   // rgb = SH coefficient, w unused
} shOut;

layout(push_constant) uniform Push {
    vec4 p;   // x = source mip, y = face size at that mip, z = ground-bounce factor, w unused
} pc;

// Cube face (Vulkan layer order: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z) + in-face uv -> dir.
// Same convention as ibl_prefilter.comp.glsl, deliberately: this must walk the
// probe in the same frame the probe was written in.
vec3 faceDir(int face, vec2 c)   // c in [-1,1]
{
    if (face == 0) return normalize(vec3( 1.0, -c.y, -c.x));
    if (face == 1) return normalize(vec3(-1.0, -c.y,  c.x));
    if (face == 2) return normalize(vec3( c.x,  1.0,  c.y));
    if (face == 3) return normalize(vec3( c.x, -1.0, -c.y));
    if (face == 4) return normalize(vec3( c.x, -c.y,  1.0));
    return               normalize(vec3(-c.x, -c.y, -1.0));   // 5 = -Z
}

shared vec3 sAcc[64 * 9];

void main()
{
    const uint tid  = gl_LocalInvocationID.x;
    const int  N    = int(pc.p.y);
    const float mip = pc.p.x;
    const uint total = uint(6 * N * N);

    vec3 L[9];
    for (int i = 0; i < 9; ++i) L[i] = vec3(0.0);

    for (uint idx = tid; idx < total; idx += 64u) {
        int face = int(idx / uint(N * N));
        uint r   = idx % uint(N * N);
        int  py  = int(r / uint(N));
        int  px  = int(r % uint(N));

        // Texel centre in [-1,1] face coords.
        vec2 c = ((vec2(px, py) + 0.5) / float(N)) * 2.0 - 1.0;
        vec3 d = faceDir(face, c);

        // Solid angle of this texel. A cube face is not an equal-area
        // parameterisation — texels near a face corner subtend far less sky than
        // texels at the face centre — so weighting them equally would tilt the
        // whole integral toward the corners. dW = (2/N)^2 / (1 + u^2 + v^2)^1.5,
        // which integrates to 4*pi over the 6 faces.
        float t  = 1.0 + c.x * c.x + c.y * c.y;
        float dW = (4.0 / float(N * N)) / (t * sqrt(t));

        vec3 rad = textureLod(uProbe, d, mip).rgb;

        // Below the horizon the sky cube holds its skirt (the horizon colour
        // smeared down), which is not sky radiance at all — the real lower
        // hemisphere is ground. Treating it as full-strength sky would light
        // every surface from underneath. Scale it by a ground-bounce albedo
        // instead: it is still the right COLOUR to bounce (the ground is lit by
        // this same sky), just far less of it.
        if (d.y < 0.0) rad *= pc.p.z;

        float x = d.x, y = d.y, z = d.z;
        L[0] += rad * (0.282095 * dW);
        L[1] += rad * (0.488603 * y * dW);
        L[2] += rad * (0.488603 * z * dW);
        L[3] += rad * (0.488603 * x * dW);
        L[4] += rad * (1.092548 * x * y * dW);
        L[5] += rad * (1.092548 * y * z * dW);
        L[6] += rad * (0.315392 * (3.0 * z * z - 1.0) * dW);
        L[7] += rad * (1.092548 * x * z * dW);
        L[8] += rad * (0.546274 * (x * x - y * y) * dW);
    }

    for (int i = 0; i < 9; ++i) sAcc[tid * 9u + uint(i)] = L[i];
    barrier();

    // Serial reduction by lane 0: 576 adds, once per weather change. Not worth a
    // tree reduction.
    if (tid == 0u) {
        for (int i = 0; i < 9; ++i) {
            vec3 s = vec3(0.0);
            for (uint t = 0u; t < 64u; ++t) s += sAcc[t * 9u + uint(i)];
            shOut.c[i] = vec4(s, 0.0);
        }
    }
}
