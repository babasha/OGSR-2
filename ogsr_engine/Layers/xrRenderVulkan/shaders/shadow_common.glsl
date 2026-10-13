// xrRenderVulkan - shared sun/spot/point shadow sampling. Was copy-pasted into
// world_lmap / world_terrain / world_vlit. #include AFTER light_ubo.glsl (reads
// L + the set-1 shadow samplers). cascTap is also used by rainVis (env_common),
// so include shadow_common BEFORE env_common.
#ifndef SHADOW_COMMON_GLSL
#define SHADOW_COMMON_GLSL

// spotLinZ + cascTap live in shadow_math.glsl: they are pure functions of their
// arguments, so vol_inject.comp (own descriptor set, own UBO) shares them from
// there rather than keeping the private copies it used to.
#include "shadow_math.glsl"

// Spot shadow POOL: project by the light's TILE view·proj, 3x3 PCF manual
// compare inside the tile's atlas rect (4x2 tiles of 1024² — see vk_shadow).
// Compared in LINEAR depth with a WORLD-space epsilon (spotLinZ).
float spotShadowF(vec3 wp, float range, int tile)
{
    vec4 c = L.spot_pool_vp[tile] * vec4(wp, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float f    = max(range, 1.0);
    float zRef = spotLinZ(ndc.z, f) - 0.08;   // 8 cm world bias, range-independent
    // Tile rect in atlas UV, inset 1.5 texels so PCF taps never leak into a
    // neighbouring tile.
    const vec2 kTileScale = vec2(0.25, 0.5);              // 1/4 cols, 1/2 rows
    vec2  texel = 1.0 / vec2(textureSize(uSpotShadow, 0));   // atlas texel
    vec2  tBase = vec2(float(tile & 3), float(tile >> 2)) * kTileScale;
    // |spot_flash.w| = the part of the tile the map was rendered into (r_spot_scale),
    // negative = the one-gather PCF (r_spot_pcf 0).
    float tScale = abs(L.spot_flash.w) > 0.01 ? abs(L.spot_flash.w) : 1.0;
    vec2  tMin  = tBase + texel * 1.5;
    vec2  tMax  = tBase + kTileScale * tScale - texel * 1.5;
    vec2  auv   = tBase + uv * (kTileScale * tScale);
    if (L.spot_flash.w < 0.0) {
        // 2x2 bilinear-weighted compare from ONE gather (cascTap's recipe) instead of
        // the 9 fetches below: the cheap receiver for the Switch. Leaves out the grass
        // blend below too (the Switch has no grass casters in the spot maps).
        vec2 sz = vec2(textureSize(uSpotShadow, 0));
        vec2 t  = auv * sz - 0.5;
        vec2 fr = fract(t);
        vec4 d  = textureGather(uSpotShadow, clamp((floor(t) + 1.0) / sz, tMin, tMax), 0);
        vec4 c  = vec4(zRef <= spotLinZ(d.x, f), zRef <= spotLinZ(d.y, f),
                       zRef <= spotLinZ(d.z, f), zRef <= spotLinZ(d.w, f));
        return mix(mix(c.w, c.z, fr.x), mix(c.x, c.y, fr.x), fr.y);
    }
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += (zRef <= spotLinZ(texture(uSpotShadow, clamp(auv + vec2(x, y) * texel, tMin, tMax)).r, f)) ? 1.0 : 0.0;
    float vis = sum * (1.0 / 9.0);
    // Grass shadows surfaces PARTIALLY: a second tap set against the spot+grass
    // beam atlas (b22), blended by L.spot_params.x. The beam tile is a superset
    // of the clean one, so its visibility is <= clean — the mix darkens the pool
    // with translucent grass dapples instead of the binary blanket (which ate
    // the headlight's ground pool at full strength) or nothing (sterile pool).
    // Flashlight tiles (handheld torch) paint CRISP grass shadows at full strength —
    // the night wow of a beam raking through grass; wide fixtures keep the subtle blend.
    uint  fmask = uint(L.spot_flash.x + 0.5);
    float k = (((fmask >> uint(tile)) & 1u) == 1u) ? L.spot_flash.y : L.spot_params.x;
    if (k > 0.001) {
        float sumG = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                sumG += (zRef <= spotLinZ(texture(uSpotShadowGrass, clamp(auv + vec2(x, y) * texel, tMin, tMax)).r, f)) ? 1.0 : 0.0;
        vis = mix(vis, sumG * (1.0 / 9.0), k);
    }
    return vis;
}

// Point POOL (cube array) shadow: 1-tap, compare D3D-style perspective depth
// along the major axis of the lookup vector (faces rendered at 90 deg, near 0.1).
// `cube` = the light's cube-array index (pointCubeOf(gi)).
float pointShadowF(vec3 wp, vec3 lp, float range, int cube)
{
    // LINEAR-depth compare with a WORLD epsilon (same fix as the spot pool): the
    // old constant 0.01 NDC bias on the 0.1-near cube projection is centimetres
    // by the fire but METRES at the range edge — an NPC standing past ~half the
    // fire's radius was lit yet cast NO shadow (his depth gap fell inside the bias).
    vec3 d = wp - lp;
    float z = max(max(abs(d.x), abs(d.y)), abs(d.z));
    const float n = 0.1;                 // kPointNear (ComputePointFaceVP)
    float f    = max(range, 1.0);
    float zRef = z - 0.08;               // 8 cm world bias, range-independent
    // 3x3 PCF, like the spot tiles: a blade by the fire projects onto a wall
    // hugely magnified, so the 512 cube's texels read as hard pixel squares
    // with 1 tap. Offsets step one cube texel in the plane perpendicular to
    // the lookup ray (90-deg face spans 2z at distance z -> texel = 2z/size).
    vec3 up = (abs(d.y) > abs(d.x) && abs(d.y) > abs(d.z)) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 t1 = normalize(cross(up, d));
    vec3 t2 = normalize(cross(d, t1));
    float texel = 2.0 * z / float(textureSize(uPointShadow, 0).x);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 dd = d + (t1 * float(x) + t2 * float(y)) * texel;
            float zMap = texture(uPointShadow, vec4(dd, float(cube))).r;
            zMap = n * f / max(f - zMap * (f - n), 1e-4);
            sum += (zRef <= zMap) ? 1.0 : 0.0;
        }
    return sum * (1.0 / 9.0);
}

// r_point_debug (L.spot_params.y): visualize the point-shadow POOL coverage on
// opaque receivers. Returns vec4(rgb, a>0 = override this pixel). GREEN = the
// pixel is inside a pooled point light's range; RED = that light's cube shadows
// it. A campfire with an NPC should paint a green pool with a red NPC shadow —
// green pool but no red = the cube has no caster; no green at all = the light/
// cube isn't reaching the receiver.
vec4 pointDebugOverlay(vec3 wp)
{
    if (L.spot_params.y < 0.5) return vec4(0.0);
    int n = int(L.counts.x + 0.5);
    float reach = 0.0, shadow = 1.0;
    for (int i = 0; i < n; ++i) {
        if (L.lights[i].color.w > 0.5) continue;   // spots handled elsewhere
        int cube = pointCubeOf(i);
        if (cube < 0) continue;
        vec3  dv = L.lights[i].pos.xyz - wp;
        float r  = L.lights[i].pos.w;
        if (dot(dv, dv) >= r * r) continue;
        reach = 1.0;
        shadow = min(shadow, pointShadowF(wp, L.lights[i].pos.xyz, r, cube));
    }
    if (reach < 0.5) return vec4(0.0);
    return vec4(mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), shadow), 1.0);
}

// cascTap → shadow_math.glsl (included above).

// One sun-cascade lookup: 2x2 spread of bilinear gather taps ~ a 3x3 smooth
// kernel. Returns the lit factor, or -1.0 when worldPos falls outside this
// cascade (1% UV inset: border bilinear taps would mix unrendered texels).
float cascSample(sampler2D smap, mat4 vp, vec3 wp, float bias_)
{
    vec3 n = (vp * vec4(wp, 1.0)).xyz;          // ortho -> already NDC (w == 1)
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.01 || uv.x > 0.99 || uv.y < 0.01 || uv.y > 0.99
        || n.z <= 0.0 || n.z >= 1.0)
        return -1.0;
    float ref = n.z - bias_;
    vec2  tx  = 1.0 / vec2(textureSize(smap, 0));
    return 0.25 * (cascTap(smap, uv + vec2(-0.5, -0.5) * tx, ref)
                 + cascTap(smap, uv + vec2( 0.5, -0.5) * tx, ref)
                 + cascTap(smap, uv + vec2(-0.5,  0.5) * tx, ref)
                 + cascTap(smap, uv + vec2( 0.5,  0.5) * tx, ref));
}

// Sun shadow, R4 cascade scheme: cascade 0 (25 m) -> cascade 1 (60 m) -> the
// cached far map (320 m). Returns lit factor 1=lit .. 0=fully shadowed.
// Out-of-map -> lit.
float sunShadow(vec3 worldPos)
{
    float s = cascSample(uShadowNear, L.sun_near_vp, worldPos, 0.0004);
    if (s >= 0.0) return s;
    s = cascSample(uShadowC1, L.sun_c1_vp, worldPos, 0.0006);
    if (s >= 0.0) return s;

    vec4 c = L.sun_vp * vec4(worldPos, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    float ref   = ndc.z - 0.0015;
    // 4 spread taps (was 3x3): world pixels dominate the frame.
    vec2  texel = 1.0 / vec2(textureSize(uShadow, 0));
    float sum = 0.0;
    sum += (ref <= texture(uShadow, uv + vec2(-0.75, -0.75) * texel).r) ? 1.0 : 0.0;
    sum += (ref <= texture(uShadow, uv + vec2( 0.75, -0.75) * texel).r) ? 1.0 : 0.0;
    sum += (ref <= texture(uShadow, uv + vec2(-0.75,  0.75) * texel).r) ? 1.0 : 0.0;
    sum += (ref <= texture(uShadow, uv + vec2( 0.75,  0.75) * texel).r) ? 1.0 : 0.0;
    return sum * 0.25;
}

#endif // SHADOW_COMMON_GLSL
