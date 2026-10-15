#version 450
// xrRenderVulkan — LOCAL FETCH: how big is the body of water under this texel.
//
// ⚠ WHY THIS EXISTS. The wave machinery already knows that a wave only exists if
// its length fits in the basin several times over — waterFit() in
// water_common.glsl gates every octave on `fetch`, and the spectral field has a
// size below which it stays out entirely. That mechanism is right. What it was
// being handed was not: `fetch` came from BasinFetch(), the bounding box of the
// water VISUAL, and level water is not modelled one mesh per pool. Cordon is one
// visual, 1952 triangles, 79.8 x 148.2 m — so every drop of water on the level,
// including the two-metre circle inside a well in the newbie village, was told it
// sat in a 108 metre basin and got the open river's swell.
//
// Measured, not argued (r_wtr_audit at that well, 16-08):
//   pool 0: 2.9 m2, y=-19.98, span 2.0 x 1.9 m, at world (-192.6, -152.0)
//   shore wetness -> 505 wet texel(s), wetY [-19.92 .. -19.55]
// The still sheet is at -19.98 and the wetness marks reach -19.55: the drawn
// surface was climbing 37 cm above its own level in a puddle 2 m across. That is
// the crest leaving the well through its stone ring, the near-field grid
// extending it into a floating square, and the contact test soaking a ring of
// yard that dried and re-wetted as the crest came and went.
//
// So this pass answers the question BasinFetch cannot: not "how big is the mesh
// this water belongs to" but "how far does open water actually extend from
// here", per texel, out of the pool mask — the one image that knows where each
// body of water ends at 12.5 cm.
//
// ⭐ IT CAN ONLY PROVE SMALLNESS. The tile is 64 m; anything that reaches the
// search limit is written as 0 = "no local answer", and the consumer keeps the
// visual's bbox fetch exactly as before. Open water is therefore untouched by
// construction, and the only thing this can do is take a wave AWAY from a body
// too small to carry it.
layout(local_size_x = 8, local_size_y = 8) in;

// The pool mask, in tile space: water surface height per texel, `dry` where there
// is none. Read as a storage image (same as the sim step) — no filtering, and a
// height field with a -10000 sentinel must never be filtered anyway.
layout(set = 0, binding = 0, r32f) uniform readonly  image2D uMask;
// The answer, coarser: metres of basin, 0 = "unknown, use the bbox".
layout(set = 0, binding = 1, r32f) uniform writeonly image2D uFetch;

layout(push_constant) uniform PC {
    float dry;          // the mask's "no water" sentinel
    float mpt;          // metres per MASK texel
    float reach;        // how far out to look (m); reaching it means "open"
    float stepM;        // walk step (m)
    int   maskTexels;
    int   fetchTexels;
    int   enable;       // 0 = write 0 everywhere: the feature is off
    int   pad;
} pc;

// Two puddles are one body only if they touch AND sit at the same level — the
// same 35 cm rule the sim and the audit use. A stepped cellar floor is a
// staircase of pools that meet in plan view and have no wave path between them.
const float kSameLevel = 0.35;

// Eight directions. Four would draw the measurement's own axes into the result
// on anything diagonal; eight is enough that the largest run is within a few per
// cent of the true one, and this quantity feeds a smoothstep, not a shoreline.
const vec2 kDir[8] = vec2[8](
    vec2( 1.0,  0.0), vec2(-1.0,  0.0), vec2( 0.0,  1.0), vec2( 0.0, -1.0),
    vec2( 0.70710678,  0.70710678), vec2(-0.70710678,  0.70710678),
    vec2( 0.70710678, -0.70710678), vec2(-0.70710678, -0.70710678));

void main()
{
    const ivec2 c = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= pc.fetchTexels || c.y >= pc.fetchTexels) return;

    if (pc.enable == 0) { imageStore(uFetch, c, vec4(0.0)); return; }

    // Centre of this (coarse) texel in MASK texel coordinates.
    const float scale = float(pc.maskTexels) / float(pc.fetchTexels);
    const vec2  m0    = (vec2(c) + 0.5) * scale;
    const float y0    = imageLoad(uMask, ivec2(m0)).r;
    // No water in this column: nothing to measure. The consumer dilates over a
    // texel or so, which is what covers the sheet's edge where the drawn surface
    // sits just outside the last wet texel.
    if (y0 <= pc.dry + 1.0) { imageStore(uFetch, c, vec4(0.0)); return; }

    float best = 0.0;
    for (int d = 0; d < 8; ++d) {
        const vec2 dir = kDir[d] / pc.mpt;      // metres -> mask texels
        float run = 0.0;
        for (float s = pc.stepM; s <= pc.reach; s += pc.stepM) {
            const ivec2 t = ivec2(m0 + dir * s);
            // Off the tile: we cannot see far enough to call this body small.
            // Treat it as open — erring the other way would flatten the sea
            // every time its far side left the tile.
            if (t.x < 0 || t.y < 0 || t.x >= pc.maskTexels || t.y >= pc.maskTexels) {
                run = pc.reach;
                break;
            }
            const float y = imageLoad(uMask, t).r;
            if (y <= pc.dry + 1.0 || abs(y - y0) > kSameLevel) break;
            run = s;
        }
        // ⭐ The MAXIMUM over directions, not the minimum. A point on the bank of
        // a big lake has dry ground on one side and forty metres of water on the
        // other, and it is the forty metres that decides what wave arrives there
        // — that is what fetch MEANS. Taking the minimum would kill the swell
        // along every shoreline, which is the one place it is most visible.
        best = max(best, run);
        if (best >= pc.reach - 0.001) { imageStore(uFetch, c, vec4(0.0)); return; }
    }

    // Open water in SOME direction for `best` metres means the body spans at
    // least twice that across — the same quantity BasinFetch estimates as
    // sqrt(area) for a visual.
    imageStore(uFetch, c, vec4(best * 2.0, 0.0, 0.0, 0.0));
}
