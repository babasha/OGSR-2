#version 450
// Depth-only dither for the cluster-LOD crossfade — no color outputs, just the
// screen-door discard. Bayer pattern MUST match the color-pass clusterDither()
// exactly (same 4x4 table, same fade decode), or depth and color disagree.
layout(location = 0) flat in uint vFadeBits;

void main()
{
    uint fA = (vFadeBits >> 20) & 63u;   // fade-in vs parent (63 = solid)
    uint fB = (vFadeBits >> 26) & 63u;   // fade-out to children (0 = none)
    if (fA < 63u || fB > 0u) {
        const float kBayer[16] = float[16](0.,8.,2.,10., 12.,4.,14.,6., 3.,11.,1.,9., 15.,7.,13.,5.);
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        float d = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        if (d >= float(fA) / 63.0 || d < float(fB) / 63.0) discard;
    }
}
