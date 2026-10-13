#version 450
// xrRenderVulkan — clustered forward light culling (vk_clustered).
//
// One thread per froxel. Build the cluster's VIEW-space AABB from its grid
// coordinates + the camera projection (tan/near/far, exponential depth slices),
// then test every active light's bounding sphere and write the overlapping
// light indices into this cluster's fixed slot region. No atomics — each cluster
// owns its [ci*MAXPER .. ci*MAXPER+MAXPER) range, so writes never collide.
//
// View-space basis matches the SSAO/shafts depth-reconstruction convention
// (camera right/up/forward unit vectors from VK::DeriveProjTerms): a point at
// ndc.y>0 lies in the +up direction, and the negative-height viewport puts that
// at the TOP of the screen (gl_FragCoord.y small) — so the fragment-side cluster
// lookup (gl_FragCoord-tiled) bins to the same cluster this builds.

layout(local_size_x = 64) in;

struct Light {
    vec4 pos;    // xyz world position, w = range
    vec4 color;  // rgb colour,        w = 1 spot / 0 point
    vec4 dir;    // xyz spot direction, w = cos(half cone)
};

layout(std430, set = 0, binding = 0) readonly  buffer Lights   { Light lights[];  };
layout(std430, set = 0, binding = 1) writeonly buffer Grid      { uint  grid[];    };  // per-cluster light count
layout(std430, set = 0, binding = 2) writeonly buffer Indices   { uint  indices[]; };  // ci*MAXPER + k → light index

layout(push_constant) uniform Push {
    vec4 eye;     // xyz camera world pos
    vec4 fwd;     // xyz camera forward (unit)
    vec4 right;   // xyz camera right   (unit)
    vec4 up;      // xyz camera up/top  (unit)
    vec4 grid;    // x=GX y=GY z=GZ w=MAXPER
    vec4 proj;    // x=tanX y=tanY z=near w=far
    vec4 misc;    // x=numLights y=log2(far/near)
} pc;

void main()
{
    uint ci = gl_GlobalInvocationID.x;
    uint GX = uint(pc.grid.x), GY = uint(pc.grid.y), GZ = uint(pc.grid.z);
    uint total = GX * GY * GZ;
    if (ci >= total) return;

    uint MAXPER = uint(pc.grid.w);
    uint cx = ci % GX;
    uint cy = (ci / GX) % GY;
    uint cz = ci / (GX * GY);

    float tanX = pc.proj.x, tanY = pc.proj.y;
    float nearZ = pc.proj.z;
    float logFN = pc.misc.y;

    // Exponential depth slice [zn, zf].
    float zn = nearZ * exp2(logFN * (float(cz)     / float(GZ)));
    float zf = nearZ * exp2(logFN * (float(cz + 1) / float(GZ)));

    // Screen-tile NDC bounds (ndc_y flipped: top tile = larger ndc_y).
    float nx0 = 2.0 * float(cx)     / float(GX) - 1.0;
    float nx1 = 2.0 * float(cx + 1) / float(GX) - 1.0;
    float ny0 = 1.0 - 2.0 * float(cy + 1) / float(GY);   // bottom edge (smaller ndc_y)
    float ny1 = 1.0 - 2.0 * float(cy)     / float(GY);   // top edge    (larger  ndc_y)

    // View-space AABB over the 4 NDC corners × {zn, zf}. view.xy = ndc * tan * z.
    vec3 bmin = vec3( 1e30);
    vec3 bmax = vec3(-1e30);
    for (int zi = 0; zi < 2; ++zi) {
        float z = (zi == 0) ? zn : zf;
        float x0 = nx0 * tanX * z, x1 = nx1 * tanX * z;
        float y0 = ny0 * tanY * z, y1 = ny1 * tanY * z;
        bmin.x = min(bmin.x, min(x0, x1)); bmax.x = max(bmax.x, max(x0, x1));
        bmin.y = min(bmin.y, min(y0, y1)); bmax.y = max(bmax.y, max(y0, y1));
        bmin.z = min(bmin.z, z);           bmax.z = max(bmax.z, z);
    }

    uint count = 0u;
    uint base  = ci * MAXPER;
    uint n     = uint(pc.misc.x);
    for (uint i = 0u; i < n; ++i) {
        vec3 rel = lights[i].pos.xyz - pc.eye.xyz;
        vec3 c   = vec3(dot(rel, pc.right.xyz), dot(rel, pc.up.xyz), dot(rel, pc.fwd.xyz));
        float r  = lights[i].pos.w;
        // Sphere vs AABB: squared distance from the sphere centre to the box.
        vec3 q = clamp(c, bmin, bmax);
        vec3 d = c - q;
        if (dot(d, d) <= r * r) {
            if (count < MAXPER) { indices[base + count] = i; count++; }
        }
    }
    grid[ci] = count;
}
