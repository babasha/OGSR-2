#version 450
// Depth-driven VRS: one invocation per ~16x16 screen tile. Reads the scene depth
// (prepass), takes the NEAREST surface over a few taps (conservative — keep a tile
// fine if ANYTHING near is in it), maps distance → fragment size, writes the rate
// byte into the shading-rate image. Far/sky tiles coarse-shade; near stays 1x1.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;                 // scene depth (D32)
layout(set = 0, binding = 1, r8ui) uniform writeonly uimage2D uRate;   // shading-rate image
// Diagnostics: per-frame tile histogram (0 = 1x1, 1 = 2x2, 2 = 4x4). CPU zeroes
// the slot before dispatch and logs it N frames later ([VK VRS] tiles ...).
layout(set = 0, binding = 2) buffer Hist { uint cnt[3]; } uHist;

layout(push_constant) uniform PC {
    vec2  invScreen;   // 1/screenW, 1/screenH
    vec2  nearFar;     // x = near dist (full 1x1 below), y = far dist (coarsest above)
    vec2  proj;        // x = p43 (A), y = p33 (B): viewZ = A / (zndc - B)
    ivec2 tiles;       // SRI dims (tilesW, tilesH)
    ivec2 texel;       // tile texel size (e.g. 16,16)
    int   level;       // 1 mild (max 2x2), 2 aggressive (far → 4x4)
} pc;

uint encRate(uint w, uint h)
{
    uint lw = (w >= 4u) ? 2u : ((w >= 2u) ? 1u : 0u);
    uint lh = (h >= 4u) ? 2u : ((h >= 2u) ? 1u : 0u);
    return (lh << 2) | lw;   // (log2(h)<<2)|log2(w)
}

float viewZ(vec2 uv)
{
    float z = texture(uDepth, uv).r;
    if (z >= 0.999990) return 1e9;      // sky / cleared → very far
    return pc.proj.x / (z - pc.proj.y);
}

void main()
{
    ivec2 t = ivec2(gl_GlobalInvocationID.xy);
    if (t.x >= pc.tiles.x || t.y >= pc.tiles.y) return;

    vec2 base = vec2(t * pc.texel);
    vec2 ts   = vec2(pc.texel);
    float nearest = 1e9;
    nearest = min(nearest, viewZ((base + ts * vec2(0.25, 0.25)) * pc.invScreen));
    nearest = min(nearest, viewZ((base + ts * vec2(0.75, 0.25)) * pc.invScreen));
    nearest = min(nearest, viewZ((base + ts * vec2(0.25, 0.75)) * pc.invScreen));
    nearest = min(nearest, viewZ((base + ts * vec2(0.75, 0.75)) * pc.invScreen));

    uint w = 1u, h = 1u;
    if (nearest > pc.nearFar.y)      { w = (pc.level >= 2) ? 4u : 2u; h = w; }
    else if (nearest > pc.nearFar.x) { w = 2u; h = 2u; }

    imageStore(uRate, t, uvec4(encRate(w, h), 0u, 0u, 0u));
    atomicAdd(uHist.cnt[(w >= 4u) ? 2u : ((w >= 2u) ? 1u : 0u)], 1u);
}
