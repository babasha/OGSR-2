// xrRenderVulkan — cheap 1-tap dynamic-light shadows for FOLIAGE (grass/trees).
// Foliage is dense/overdrawn, so it samples the spot atlas / point cube ONCE
// (no 3x3 PCF — that lives in shadow_common.glsl for opaque receivers). This
// makes NPCs/props around a campfire or under a lamp cast onto the grass, not
// just the terrain. Uses texture() (implicit LOD) → FRAGMENT stage only, so it
// lives here and NOT in the stage-shared light_ubo.glsl. #include AFTER
// light_ubo.glsl (needs L + uSpotShadow/uPointShadow + spotTileOf/pointCubeOf).
#ifndef FOLIAGE_SHADOW_GLSL
#define FOLIAGE_SHADOW_GLSL

float spotShadow1(vec3 wp, float range, int tile)
{
    vec4 c = L.spot_pool_vp[tile] * vec4(wp, 1.0);
    if (c.w <= 0.0) return 1.0;
    vec3 ndc = c.xyz / c.w;
    vec2 uv = ndc.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0) return 1.0;
    const vec2 kTileScale = vec2(0.25, 0.5);       // 4 cols, 2 rows
    vec2  tBase = vec2(float(tile & 3), float(tile >> 2)) * kTileScale;
    float tScale = abs(L.spot_flash.w) > 0.01 ? abs(L.spot_flash.w) : 1.0;   // r_spot_scale (shadow_common.glsl)
    vec2  auv   = tBase + clamp(uv, 0.0, 1.0) * (kTileScale * tScale);
    float f = max(range, 1.0); const float n = 0.5;
    float zRef = n * f / max(f - ndc.z * (f - n), 1e-4) - 0.08;
    float zMap = texture(uSpotShadow, auv).r;
    zMap = n * f / max(f - zMap * (f - n), 1e-4);
    return (zRef <= zMap) ? 1.0 : 0.0;
}
float pointShadow1(vec3 wp, vec3 lp, float range, int cube)
{
    // LINEAR-depth compare, world epsilon (same fix as the spot pool) — the old
    // 0.01 NDC bias grew to metres at the range edge and ate far NPC shadows.
    vec3 d = wp - lp;
    float z = max(max(abs(d.x), abs(d.y)), abs(d.z));
    const float n = 0.1;                 // kPointNear
    float f = max(range, 1.0);
    float zMap = texture(uPointShadow, vec4(d, float(cube))).r;
    zMap = n * f / max(f - zMap * (f - n), 1e-4);
    return (z - 0.08 <= zMap) ? 1.0 : 0.0;
}
// One dynamic light's shadow factor for foliage (spot tile OR point cube).
float foliageLightShadow(int i, vec3 wp)
{
    if (L.lights[i].color.w > 0.5) {
        int tile = spotTileOf(i);
        return (tile >= 0) ? spotShadow1(wp, L.lights[i].pos.w, tile) : 1.0;
    }
    int cube = pointCubeOf(i);
    return (cube >= 0) ? pointShadow1(wp, L.lights[i].pos.xyz, L.lights[i].pos.w, cube) : 1.0;
}

// r_point_debug (L.spot_params.y) overlay for foliage — matches the opaque one
// in shadow_common.glsl so grass/tree blades show the SAME green(lit)/red(shadow)
// as the ground. a>0 = override the blade colour.
vec4 foliagePointDebug(vec3 wp)
{
    if (L.spot_params.y < 0.5) return vec4(0.0);
    int n = int(L.counts.x + 0.5);
    float reach = 0.0, shadow = 1.0;
    for (int i = 0; i < n; ++i) {
        if (L.lights[i].color.w > 0.5) continue;
        int cube = pointCubeOf(i);
        if (cube < 0) continue;
        vec3  dv = L.lights[i].pos.xyz - wp;
        float r  = L.lights[i].pos.w;
        if (dot(dv, dv) >= r * r) continue;
        reach = 1.0;
        shadow = min(shadow, pointShadow1(wp, L.lights[i].pos.xyz, r, cube));
    }
    if (reach < 0.5) return vec4(0.0);
    return vec4(mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), shadow), 1.0);
}

// Dynamic point/spot accumulation for FOLIAGE. Attenuation-only (leaves and
// blades carry no per-pixel normal, so there is no N-L term) + the 1-tap shadow
// above. The opaque receivers use light_shade.glsl::dynLights instead; the beam
// falloff/cone maths is kept in step with it deliberately.

// Foliage variant: leaves have no per-pixel normal -> attenuation-only.
vec3 dynLightsFoliage(vec3 wp)
{
    vec3 acc = vec3(0.0);
    int n = int(L.counts.x + 0.5);
    for (int i = 0; i < n; ++i) {
        vec3  dv = L.lights[i].pos.xyz - wp;
        float r  = L.lights[i].pos.w;
        float d2 = dot(dv, dv);
        if (d2 >= r * r) continue;
        float d   = sqrt(max(d2, 1e-6));
        // Narrow beams: windowed falloff (far half still lights) — light_shade.glsl.
        float att;
        if (L.lights[i].color.w > 0.5 && L.lights[i].dir.w > 0.87) {
            att = 1.0 - (d2 / (r * r));
            att *= att;
        } else {
            att = 1.0 - d / r;
            att *= att;
        }
        if (L.lights[i].color.w > 0.5) {
            // Narrow beams: full inside the cone + spill to 2x the angle — see
            // light_shade.glsl (axis-peaked ramp left beam-lit foliage dark).
            float ca = dot(-dv / d, L.lights[i].dir.xyz);
            float ci = L.lights[i].dir.w;
            if (ci > 0.87) {
                float co = 2.0 * ci * ci - 1.0;
                att *= clamp((ca - co) / max(ci - co, 1e-3), 0.0, 1.0);
            } else
                att *= clamp((ca - ci) / max(1.0 - ci, 1e-3), 0.0, 1.0);
        }
        // Dynamic shadow (spot tile / point cube): NPCs/props around a campfire or
        // under a lamp cast onto the foliage, not just the terrain.
        att *= foliageLightShadow(i, wp);
        acc += L.lights[i].color.rgb * (att * 0.7);
    }
    return acc;
}

#endif // FOLIAGE_SHADOW_GLSL
