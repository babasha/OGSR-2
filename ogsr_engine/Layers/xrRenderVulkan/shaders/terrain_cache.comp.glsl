#version 450
// TERRAIN COMPOSITE CACHE bake (r_terra_cache). Once per camera-window move,
// composites the 4-channel splat ground into a camera-anchored world-space
// cache: R16F HEIGHT (mask+height-blend+CH_OFF resolved) + RGBA8 BLEND WEIGHTS.
// The color pass and the depth prepass then march ONE texture instead of four
// (4 marches -> 1, all in-march taps 4x -> 1x) and blend the native-res detail
// diffuse/normals by the baked weights. Albedo/normal stay native-res taps in
// the fragment shaders - the cache only owns the RELIEF + the weights, so no
// close-up crispness is lost.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 1)  uniform sampler2D uMask;   // RGBA splat weights (terrain set)
layout(set = 0, binding = 11) uniform sampler2D uDhR;    // grass   height
layout(set = 0, binding = 12) uniform sampler2D uDhG;    // asphalt height
layout(set = 0, binding = 13) uniform sampler2D uDhB;    // earth   height
layout(set = 0, binding = 14) uniform sampler2D uDhA;    // gravel/mud height

// RGBA16F: .r = composite height (this pass), .g = cone-step ratio + .b = sun
// horizon slope (written by the terrain_cache_cone pass right after; zeroed
// here so a disabled cone pass degrades to "no leap / no shadow", not garbage).
layout(set = 1, binding = 0, rgba16f) uniform writeonly image2D outHeight;
layout(set = 1, binding = 1, rgba8) uniform writeonly image2D outWeights;

layout(push_constant) uniform Push {
    vec4 originExtent;   // x,y = cache world origin XZ; z = world extent (m); w = height-tap lod
    vec4 uvU;            // base/mask u = dot(uvU.xy, world.xz) + uvU.z (full affine, mapping may be rotated)
    vec4 uvV;            // base/mask v = dot(uvV.xy, world.xz) + uvV.z
    vec4 chOff;          // per-channel height offsets (SSFX ssfx_terrain_offset)
    vec4 dsPad;          // x = detailScale (detail uv = base uv * ds); y = r_terra_blend
    vec4 tmask;          // BAKED world-space splat mask (mask-less maps): maskUV =
                         // (wxz - xy) * zw; .z == 0 -> the material's own mask at `uv`
} pc;

void main()
{
    ivec2 res = imageSize(outHeight);
    ivec2 px  = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= res.x || px.y >= res.y) return;

    vec2 wxz = pc.originExtent.xy + (vec2(px) + 0.5) / vec2(res) * pc.originExtent.z;
    vec2 uv  = vec2(dot(pc.uvU.xy, wxz) + pc.uvU.z, dot(pc.uvV.xy, wxz) + pc.uvV.z);
    vec2 duv = uv * pc.dsPad.x;
    float lod = pc.originExtent.w;

    // Splat mask, normalized (same rules as world_terrain.frag main() — including
    // the world-space BAKED mask on mask-less maps, matching terrainMaskUV()).
    vec2 muv  = (pc.tmask.z > 0.0) ? (wxz - pc.tmask.xy) * pc.tmask.zw : uv;
    vec4 mask = textureLod(uMask, muv, 0.0);
    float wsum = dot(mask, vec4(1.0));
    mask = (wsum > 1e-4) ? (mask / wsum) : vec4(1.0, 0.0, 0.0, 0.0);

    vec4 chH = vec4(clamp(textureLod(uDhR, duv, lod).r + pc.chOff.x, 0.0, 1.0),
                    clamp(textureLod(uDhG, duv, lod).r + pc.chOff.y, 0.0, 1.0),
                    clamp(textureLod(uDhB, duv, lod).r + pc.chOff.z, 0.0, 1.0),
                    clamp(textureLod(uDhA, duv, lod).r + pc.chOff.w, 0.0, 1.0));

    // Blend weights (same r_terra_blend rule as world_terrain.frag): 0 = plain
    // mask cross-fade - the GAMMA/SSFX look (their HeightBlending() ships
    // commented out; asphalt fades smoothly into soil, mix-zone relief is the
    // soft average); >0 = Mishkinis height blend - the RAISED material wins
    // per-texel, sharp interlocking edges + full relief in mix zones.
    vec4 bw = mask;
    if (pc.dsPad.y > 0.005) {
        float blendDepth = pc.dsPad.y;
        vec4  wh   = mask + chH;
        float maxw = max(max(wh.x, wh.y), max(wh.z, wh.w));
        vec4  b    = max(wh - (maxw - blendDepth), 0.0) * step(vec4(1e-4), mask);
        float bsum = dot(b, vec4(1.0));
        if (bsum > 1e-5) bw = b / bsum;
    }
    float H = dot(chH, bw);   // the composite SURFACE the fragment shaders march

    imageStore(outHeight,  px, vec4(H, 0.0, 0.0, 0.0));
    imageStore(outWeights, px, bw);
}
