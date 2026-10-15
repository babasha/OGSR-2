// xrRenderVulkan - PARKED water-flow-sim sampling helpers (r_water_sim, off by
// default). Sample the sim's depth/velocity (set 1 bindings 11/12, aligned to
// rain_vp) + the rain-map ground height. Was copy-pasted into world_lmap /
// world_terrain / world_vlit. #include AFTER light_ubo.glsl.
#ifndef FLOW_SIM_SAMPLE_GLSL
#define FLOW_SIM_SAMPLE_GLSL

// Water DEPTH (metres) from the flow sim, sampled via rain_vp. 0 where dry.
float simWater(vec3 wp)
{
    vec4 c = L.rain_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) return 0.0;
    vec2 uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
    return textureLod(uWater, uv, 0.0).r;
}

// Blurred water depth for puddle placement (spreads crease/seam line-pooling into
// smooth area puddles).
float simWaterSoft(vec3 wp)
{
    vec4 c = L.rain_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) return 0.0;
    vec2 uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
    vec2 px = 5.0 / vec2(textureSize(uWater, 0));
    return textureLod(uWater, uv, 0.0).r * 0.4
         + (textureLod(uWater, uv + vec2(px.x, 0.0), 0.0).r
          + textureLod(uWater, uv - vec2(px.x, 0.0), 0.0).r
          + textureLod(uWater, uv + vec2(0.0, px.y), 0.0).r
          + textureLod(uWater, uv - vec2(0.0, px.y), 0.0).r) * 0.15;
}

// Water VELOCITY (uv/sec) from the flow sim -> drives the moving-water surface.
vec2 simFlow(vec3 wp)
{
    vec4 c = L.rain_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) return vec2(0.0);
    vec2 uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec2(0.0);
    return vec2(textureLod(uFlow, uv, 0.0).r, -textureLod(uFlow, uv, 0.0).g);  // uv-vel -> world XZ (Z flipped)
}

// Ground height (metres, relative) from the rain map ortho depth - flow debug.
float groundHm(vec3 wp)
{
    vec4 c = L.rain_vp * vec4(wp, 1.0);
    if (c.w <= 0.0) return 0.0;
    vec2 uv = c.xy * 0.5 + 0.5; uv.y = 1.0 - uv.y;
    return -textureLod(uRainMap, uv, 0.0).r * 349.0;
}

// Water debug colour. 1 = DEPTH ramp, 2 = FLOW direction (downhill gradient).
vec3 waterDebugColor(vec3 wp, int mode)
{
    float d = simWater(wp);
    if (mode == 2) {
        float e = 0.6;
        float sx1 = groundHm(wp + vec3( e,0,0)) + simWater(wp + vec3( e,0,0));
        float sx0 = groundHm(wp + vec3(-e,0,0)) + simWater(wp + vec3(-e,0,0));
        float sz1 = groundHm(wp + vec3(0,0, e)) + simWater(wp + vec3(0,0, e));
        float sz0 = groundHm(wp + vec3(0,0,-e)) + simWater(wp + vec3(0,0,-e));
        vec2 flow = -vec2(sx1 - sx0, sz1 - sz0);
        float sp  = clamp(length(flow) * 2.0, 0.0, 1.0);
        vec2 dir  = (length(flow) > 1e-5) ? normalize(flow) : vec2(0.0);
        vec3 c = vec3(dir * 0.5 + 0.5, 0.3) * sp;
        return (d > 0.005) ? c : c * 0.15;
    }
    float t = clamp(d / 0.6, 0.0, 1.0);
    vec3 c = vec3(0.0, t * 0.55, t) + vec3(smoothstep(0.75, 1.0, t));
    return (d < 0.005) ? vec3(0.02) : c;
}

// Travelling surface waves ALONG the water flow (downhill). xz normal perturbation.
vec2 flowWaves(vec3 wp, float t)
{
    float d = simWater(wp);
    if (d < 0.003) return vec2(0.0);
    float e = 0.6;
    float sx1 = groundHm(wp + vec3( e,0,0)) + simWater(wp + vec3( e,0,0));
    float sx0 = groundHm(wp + vec3(-e,0,0)) + simWater(wp + vec3(-e,0,0));
    float sz1 = groundHm(wp + vec3(0,0, e)) + simWater(wp + vec3(0,0, e));
    float sz0 = groundHm(wp + vec3(0,0,-e)) + simWater(wp + vec3(0,0,-e));
    vec2 flow = -vec2(sx1 - sx0, sz1 - sz0);
    float spd = length(flow);
    if (spd < 1e-4) return vec2(0.0);
    vec2 dir = flow / spd;
    float along = dot(wp.xz, dir);
    float w = sin(along * 7.0  - t * (2.0 + spd * 30.0))
            + 0.5 * sin(along * 16.0 - t * (3.5 + spd * 50.0) + 1.3);
    float amp = clamp(spd * 6.0, 0.0, 1.0) * clamp(d * 8.0, 0.0, 1.0);
    return dir * (w * amp * 0.5);
}

#endif // FLOW_SIM_SAMPLE_GLSL
