#version 450
// xrRenderVulkan — the final gather, looked at directly (r_gather_debug).
//
// Without this the pass is unfalsifiable: gathered indirect light lands inside
// the ambient term of three receivers, multiplied by albedo, a lightmap and an
// AO — every one of which can hide a broken gather or fake a working one. The
// modes exist to separate the three things that actually go wrong:
//
//   1  RESOLVED irradiance — what the receivers will sample. Is it the right
//      COLOUR and the right BRIGHTNESS (it must look like the probe field, only
//      sharper — if it is much darker or much brighter, the units drifted).
//   2  RAW irradiance, before the denoiser. Is the noise level something a
//      3x3/5x5 + temporal can eat, or is the estimator itself broken?
//   3  Mean HIT DISTANCE, false-coloured. Are the rays actually finding the
//      world (near = red, far/sky = blue)? A uniformly blue frame means every
//      ray escapes, which is what an empty or mis-placed BVH looks like — and
//      that is indistinguishable from "the GI is subtle" in mode 1.
//   4  The 2-band SH's DIRECTION (r_gather_sh), as a colour: where the light this
//      pixel gathered is coming FROM. Grey = no preferred direction at all.
//   5  Its STRENGTH, |d| / L0 — how much a normal-mapped facet can swing this
//      pixel's ambient. Black = a flat answer (the SH is doing nothing here and
//      the pixel behaves exactly as it did without it), white = the full swing
//      the limiter allows. This is the view that says whether the SH is worth its
//      memory in a given scene, which mode 1 cannot: the two look identical there
//      by construction, because L0 is unchanged.
//   6  The ACCUMULATED FRAME COUNT (r_gather_adapt), false-coloured. This is the
//      view that says whether the progressive alpha is doing what it claims, and
//      it answers a question no other mode can: is a pixel young because it
//      genuinely just appeared, or because the reprojection test keeps throwing
//      its history away? RED = 1 (reset every frame — the history is never
//      accepted), GREEN = climbing, WHITE = saturated at 1/(1-alpha), i.e. this
//      pixel has handed over to the exponential window and behaves EXACTLY as it
//      did before the feature existed.
//      ⚠⚠ A frame that stays red while the player STANDS STILL is a broken test,
//      not a fast camera — and it would look like "the gather went flat", because
//      an alpha pinned near 1/2 leaves the spatial blur carrying the whole
//      denoise instead of the temporal accumulation.

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uResolved;
layout(set = 0, binding = 1) uniform sampler2D uRaw;
layout(set = 0, binding = 2) uniform sampler2D uSH;   // resolved L1, 3 tiles (R, G, B channel)
layout(set = 0, binding = 3) uniform sampler2D uAge;  // accumulated frame count (mode 6)

layout(push_constant) uniform PC {
    vec4 params;   // x = mode, y = 1/screenW, z = 1/screenH, w = ray length (m), for the distance ramp
    vec4 age;      // x = the count mode 6 normalises against, 1/(1-alpha)
} pc;

void main()
{
    const vec2 uv   = gl_FragCoord.xy * pc.params.yz;
    const int  mode = int(pc.params.x);

    if (mode == 3) {
        // ⚠From uRaw, not uResolved: the resolved alpha now carries view depth
        // (the upsample needs it). The raw buffer is where the hit distance was
        // measured anyway, so this view got MORE honest, not less.
        const float d = texture(uRaw, uv).a / max(pc.params.w, 0.01);
        // near -> red, mid -> green, far/sky -> blue
        const vec3 c = clamp(vec3(1.0 - d * 2.0,
                                  1.0 - abs(d - 0.5) * 2.0,
                                  d * 2.0 - 1.0), 0.0, 1.0);
        outColor = vec4(c, 1.0);
        return;
    }

    if (mode == 6) {
        // texelFetch, exactly as the resolve reads it: a FILTERED sample count
        // would invent intermediate ages along every edge and make a hard reset
        // read as a gentle ramp — the one thing this view exists to tell apart.
        const ivec2 ts = textureSize(uAge, 0);
        const ivec2 p  = clamp(ivec2(uv * vec2(ts)), ivec2(0), ts - 1);
        const float n  = texelFetch(uAge, p, 0).r;
        const float k  = clamp((n - 1.0) / max(pc.age.x - 1.0, 1.0), 0.0, 1.0);
        const vec3  c  = (k < 0.5) ? mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), k * 2.0)
                                   : mix(vec3(0.0, 1.0, 0.0), vec3(1.0), (k - 0.5) * 2.0);
        outColor = vec4(c, 1.0);
        return;
    }

    if (mode >= 4) {
        // Luminance-weighted sum of the three channel deviations: the direction the
        // EYE would notice, rather than three separate ones it cannot compare.
        const ivec2 ts = textureSize(uResolved, 0);
        const ivec2 p  = clamp(ivec2(uv * vec2(ts)), ivec2(0), ts - 1);
        const vec3  dL = texelFetch(uSH, p,                             0).xyz * 0.2126
                       + texelFetch(uSH, p + ivec2(0, ts.y),            0).xyz * 0.7152
                       + texelFetch(uSH, p + ivec2(0, ts.y * 2),        0).xyz * 0.0722;
        const vec3  c  = texture(uResolved, uv).rgb;
        const float l0 = dot(c, vec3(0.2126, 0.7152, 0.0722));
        if (mode == 4) {
            outColor = vec4(0.5 + 0.5 * normalize(dL + vec3(1e-9)), 1.0);
        } else {
            const float k = clamp(length(dL) / max(l0, 1e-5), 0.0, 1.0);
            outColor = vec4(vec3(k), 1.0);
        }
        return;
    }

    const vec3 v = (mode == 2) ? texture(uRaw, uv).rgb : texture(uResolved, uv).rgb;
    outColor = vec4(v, 1.0);
}
