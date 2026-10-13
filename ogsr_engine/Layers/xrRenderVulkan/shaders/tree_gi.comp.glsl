#version 450
#extension GL_GOOGLE_include_directive : require

// xrRenderVulkan — PER-TREE probe-field lookup (r_tree_gi_inst).
//
// WHY THIS PASS EXISTS — it is the answer to a measurement, not a refactor.
// VK_KHR_pipeline_executable_properties (r_pipe_stats / vk_pipestats) printed the
// crown pipelines side by side and put the cost somewhere nobody was looking:
//
//   crown SHIPPING              VS Register Count=56  Binary Size=20224  FS inputs=16
//   crown TERMS 0x7DF (gi off)  VS Register Count=32  Binary Size= 5376  FS inputs=13
//
// Clearing the gi bit does not just remove a fragment term — the driver sees that
// the FS no longer consumes vGI.rgb and deletes ddgiFieldWeighted from the VERTEX
// shader, which is four fifths of that shader's binary. That is where the 0.45 ms
// lived. The screen-space gather tap the investigation started on is worth nothing
// (a build with it removed, r_tree_gather 0, moved the zone by 0.03 ms).
//
// And the vertex is the wrong RATE for this quantity. ddgiSampleIrradiance walks
// eight probes with two atlas taps each, per cascade — ~16 taps — and a canopy
// draws on the order of a million vertices. The field itself is low-frequency:
// probe spacing is metres, a crown is a few metres. So sample it ONCE PER TREE
// here, on the order of a thousand invocations, and let the vertex shader read a
// buffer. Same quantity, three orders of magnitude fewer taps.
//
// THE VERTICAL GRADIENT IS KEPT ON PURPOSE. tree.vert's comment calls it out —
// the interpolated field is what gives a crown its sense of depth, lit on top and
// dark underneath — so this samples TWO points per tree, at the bottom and top of
// the crown sphere, and the vertex shader lerps between them by height. Two taps
// per tree still beats sixteen per vertex by any measure, and the picture the A/B
// compares is then the same picture.

// ---- set 0: our own data ---------------------------------------------------
// Mirrors VK::GpuTreeMeta. Only the sphere is read here; the rest is declared so
// the std430 offsets match the buffer the cull compute shares.
struct TreeMeta {
    vec3  sphere_P;
    float sphere_R;
    uint  index_count;
    uint  ib_first;
    uint  first_vertex;
    uint  flags;
};
layout(set = 0, binding = 0, std430) readonly buffer MetaBuf { TreeMeta meta[]; };

// Three vec4 per tree, laid out so the vertex shader reads exactly what it needs:
//   [3i+0] rgb = field at the crown BOTTOM, w = field weight k
//   [3i+1] rgb = field at the crown TOP,    w = unused (kept for alignment)
//   [3i+2] x = bottom world Y, y = 1/(top-bottom), zw = unused
// The mapping constants travel WITH the answer rather than being recomputed in the
// vertex shader from the metadata: the VS does not bind the metadata buffer, and
// a second source for "where is this crown" is a second thing that can disagree.
layout(set = 0, binding = 1, std430) writeonly buffer GIBuf { vec4 gi[]; };

// ---- set 1: the shared EnvLight/DDGI set (bound as set 2 by the graphics path) --
#define ENV_SET 1
#define DDGI_COMPUTE            // texture() -> textureLod(): compute has no derivatives
#include "light_ubo.glsl"       // L.ddgi_params + L.eye_pos
#include "ddgi_field.glsl"      // ddgiFieldWeighted

layout(push_constant) uniform PC {
    uint  count;      // trees in the level (invocations)
    float maxDist;    // skip anything past this — a tree that is not drawn needs no field
    float pad0, pad1;
} pc;

layout(local_size_x = 64) in;

void main()
{
    const uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;

    const vec3  c = meta[i].sphere_P;
    const float r = max(meta[i].sphere_R, 0.01);

    // DISTANCE GATE FIRST, and it is what makes the pass cheap: 193 k trees are
    // walked, but only the ~1 k inside the draw distance pay for the atlas walk.
    // A stale entry for a tree nobody draws is harmless by construction — the
    // vertex shader only reads entries of trees that survived the cull.
    // ⚠ Compared against the crown's NEAR edge (centre minus radius), the same way
    // the cull decides visibility: gating on the centre would drop the field for a
    // big crown whose canopy is on screen while its origin is not.
    if (distance(c, L.eye_pos.xyz) - r > pc.maxDist) return;

    const float hy = r * 0.6;                 // sample the crown, not its bounding extremes
    const vec3  lo = c - vec3(0.0, hy, 0.0);
    const vec3  hi = c + vec3(0.0, hy, 0.0);
    const vec3  up = vec3(0.0, 1.0, 0.0);     // a crown card has no usable normal — sampled UP,
                                              // exactly as tree.vert and detail.frag already do

    const vec4 fLo = ddgiFieldWeighted(lo, up, normalize(L.eye_pos.xyz - lo));
    const vec4 fHi = ddgiFieldWeighted(hi, up, normalize(L.eye_pos.xyz - hi));

    gi[3u * i + 0u] = vec4(fLo.rgb, fLo.w);
    gi[3u * i + 1u] = vec4(fHi.rgb, 0.0);
    gi[3u * i + 2u] = vec4(lo.y, 1.0 / max(2.0 * hy, 1e-3), 0.0, 0.0);
}
