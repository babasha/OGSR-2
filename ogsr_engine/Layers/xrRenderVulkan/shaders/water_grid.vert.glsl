#version 450
// xrRenderVulkan — WATER: the DENSE NEAR-FIELD GRID.
//
// ⚠ Why this exists at all. Stock level water is not a grid — this level's sheet
// is 138 triangles over 962 x 792 metres, so one edge can be two hundred metres
// long. Tessellation caps at 64 subdivisions per edge in hardware, which on a
// 200 m edge is THREE METRES per segment: enough to make the 28 m swell lean, and
// nowhere near enough for a wave with a shape. No amount of shading fixes that,
// because there is no geometry to shade.
//
// So the near field gets a mesh of its own: a grid locked to the camera, at the
// SAME origin and size as the ripple tile, which means the pool mask is a
// per-texel answer to "is there water here and at what height" in exactly this
// shader's coordinates. No vertex buffer and no index buffer — every position is
// derived from gl_VertexIndex, so there is nothing to keep in sync and nothing to
// upload. The far field carries on being drawn by the level's own polygons, with
// a cross-fade at the grid border (see waterGridWeight in water.frag).
//
// This stage DISPLACES, unlike water.vert: the grid is already fine enough that
// tessellating it again would be spending a pipeline stage on a factor of one.
#extension GL_GOOGLE_include_directive : require
#include "water_common.glsl"
#include "water_shore.glsl"

// uPoolMask (the tile's still surface height) is declared by water_shore.glsl —
// the break needs it for the same subtraction this stage needs it for, and two
// declarations of one binding is a compile error.
// Shelter, so a flooded cellar under this grid does not get the open marsh's
// swell. Same query the surface and the tessellation stage make.
layout(set = 1, binding = 9) uniform sampler2D uRainMap;

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 basin;  // x = fetch (m), y = 1 (this IS the grid), z = quads per side
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

// Two triangles per quad, wound CCW. Culling is off for water anyway (you can
// stand under a surface in a flooded basement and look up at it).
const ivec2 kCorner[6] = ivec2[6](ivec2(0, 0), ivec2(1, 0), ivec2(0, 1),
                                  ivec2(1, 0), ivec2(1, 1), ivec2(0, 1));

void main()
{
    const int N  = max(int(pc.basin.z + 0.5), 1);
    const int q  = gl_VertexIndex / 6;
    const int c  = gl_VertexIndex - q * 6;
    const ivec2 qi = ivec2(q % N, q / N);
    const vec2  g  = vec2(qi + kCorner[c]) / float(N);      // 0..1 across the tile
    const vec2  xz = W.p6.yz + g * W.p6.w;

    // ⚠ IS THERE ANY WATER IN THIS QUAD AT ALL? The fallback height below exists to
    // span a WATERLINE — but it is a single tile-wide constant, so where the mask is
    // dry EVERYWHERE it stops being "keep the mesh whole across the shore" and
    // becomes "lay a sheet at the nearest body's level over all 64 m". Cordon is the
    // level that showed it: the newbie village sits inside the plan-view bounds of
    // the river without a drop of water in it, and the whole village drew knee-deep,
    // cellars included. The tile there was 0.33% water (851 wet texels of 262144) and
    // the grid painted 100% of it.
    //
    // Probed at the QUAD's four corners, not this vertex's own, so all six vertices
    // of the quad reach the same verdict and the quad collapses as a unit — a
    // zero-area triangle rasterizes nothing, which is the one way to remove it
    // without dragging a vertex down and painting the spike the note below warns of.
    // Quads that touch water at any corner are KEPT WHOLE, so every real shoreline
    // still gets its continuous mesh and the per-pixel depth fade decides the edge
    // exactly as before. Only open ground disappears.
    {
        const vec2  q0   = W.p6.yz + vec2(qi) / float(N) * W.p6.w;
        const float step = W.p6.w / float(N);
        bool anyWet = false;
        for (int i = 0; i < 4; ++i)
            if (waterShoreProbe(q0 + vec2(float(i & 1), float(i >> 1)) * step).z > 0.5) {
                anyWet = true;
                break;
            }
        if (!anyWet) {
            // All six vertices of the quad land on the SAME clip position, so both
            // triangles have zero area and rasterize nothing.
            // ⚠ w MUST be 1, not 0. vec4(0.0) looks like a harmless "nowhere", but
            // w=0 is not a point at all — the perspective divide is 0/0, the vertex
            // comes out NaN, and a NaN triangle is undefined behaviour the driver is
            // free to turn into a hang (it did: the game froze within ~90 s).
            gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
            vWorldPos   = vec3(0.0);
            vNormal     = vec3(0.0, 1.0, 0.0);
            vUV         = vec2(0.0);
            return;
        }
    }

    // Height from the tile's STILL level. Texels it knows nothing about fall back
    // to the reference sheet height (W.p7.w, the level's water plane nearest the
    // camera) so the mesh stays CONTINUOUS — a collapsed vertex would drag a
    // triangle down to the sentinel and paint a spike across the shore. What the
    // water actually covers is decided per pixel by the depth fade in the fragment
    // stage, which is finer than any mesh could be.
    const vec3 tile0 = waterShoreProbe(xz);
    float y = (tile0.z > 0.5) ? tile0.x : W.p7.w;

    vec3 wp = vec3(xz.x, y, xz.y);

    // DISPLACE. Fades out over the outer eighth of the tile so the grid meets the
    // flat far-field sheet at the same height it does — a step there would show as
    // a ring of shoreline around the player at exactly the tile radius.
    const vec2  e    = min(g, vec2(1.0) - g);
    const float edge = smoothstep(0.0, 0.12, min(e.x, e.y));
    const float calm = waterShelter(uRainMap, wp);
    // ⚠ The fetch pushed with the draw is the span of the VISUAL, and a level's
    // water is one visual: the circle inside a well and the river it shares a mesh
    // with were handed the same 108 metres, so the well carried the river's swell
    // and lifted clear of its own stone ring — which this grid then extended half
    // a metre further and drew as a square floating over the yard. Measured per
    // texel out of the pool mask, that same well reads 2 m and goes flat.
    const float fetch = waterLocalFetch(wp.xz, pc.basin.x);
    if (W.p5.w > 0.0 && edge > 0.0) {
        const float dist = distance(wp, W.p8.xyz);
        const vec4  f    = waterField(wp.xz, dist, fetch, calm);
        wp.y += f.x * W.p5.w * edge;
    }
    // THE BREAK, as geometry. The crest has to be a shape you can see against the
    // sky and against the bank — a wave you can only read from its shading is the
    // thing this whole grid exists to stop being true.
    if (W.p11.w > 0.0 && edge > 0.0) {
        const vec3 probe = waterShoreProbe(xz);
        if (probe.z > 0.5) {
            const float still = probe.x, gy = probe.y;
            const float dep   = still - gy;
            wp.y += waterShoreBreak(dep, W.p0.x, W.p11.z, W.p10.x, W.p10.w, W.p11.x).x * edge;

            // ⚠⚠ AND THEN IT HAS TO LIE ON THE SAND. Raising the sheet by the
            // run-up and leaving it flat makes a PLANE held up in the air, which
            // then intersects an uneven bank along a ragged line — a bank strewn
            // with white polygonal slabs, metres up the slope. ("Супер ужасно.")
            //
            // Above the waterline the water is a FILM ON THE GROUND: its surface
            // follows the terrain plus a couple of centimetres, and where the wave
            // has not reached it stays at the still level, which is under the
            // ground and therefore depth-tested away. min() gives exactly that —
            // the sheet climbs while it can and sinks out of sight when it cannot.
            const float onBeach = smoothstep(-0.05, 0.20, gy - still);
            wp.y = mix(wp.y, min(wp.y, gy + W.p11.w), onBeach);
        }
    }

    // CHOPPINESS, and it goes LAST — after the run-up has decided how high this
    // vertex sits and after the beach clamp has put it back on the sand. Moving a
    // vertex sideways cannot change which of those applied to it; doing the chop
    // first and the clamp second would drag the shoreline sideways with the wave.
    //
    // ⭐ THIS is the sharp crest. Everything else in this file moves a vertex UP;
    // this pulls the water either side of a peak IN toward it, which is what
    // makes the top pointed and the trough flat. It is the whole reason the
    // spectral field is here — the analytic sum of sines cannot do it at any
    // amplitude, because the top of a sine is round no matter how tall it is.
    if (edge > 0.0)
        wp.xz += waterFFTChop(wp.xz, fetch, calm) * edge;

    vWorldPos   = wp;
    vNormal     = vec3(0.0, 1.0, 0.0);   // the FS builds the real normal from the wave slope
    vUV         = xz * 0.05;
    gl_Position = pc.mvp * vec4(wp, 1.0);
}
