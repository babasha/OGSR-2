#version 450
// LEAN tonemap composite (Switch / r_tonemap_lean 1). The full tonemap.frag.glsl spends ~1.7 ms of its 2.2 ms at 720p on code
// that is dead when the effects are off (SSR puddles, water murk, lens drops, froxel volume, bloom add, CAS, debug views) but
// is still executed: the branches are dynamically uniform, yet the register footprint and the set-1 EnvLight UBO read stay.
// This variant keeps only: exposure, scene fetch, CDL grade, normalised Reinhard, gamma, TPDF dither.
// Same descriptor set 0 and push-constant block as tonemap.frag.glsl so it plugs into the same pipeline layout.
layout(set = 0, binding = 0) uniform sampler2D uHDR;
layout(set = 0, binding = 6) uniform sampler2D uResolved;

layout(push_constant) uniform PC {
    vec4 p0;   // x=whitePoint, y=topMipLOD, z=middleGray, w=lowLum
    vec4 p1;   // x=expMin, y=expMax, z=expComp, w=bloomIntensity (ignored)
    vec4 p2;   // x=cdlSlope, y=cdlSaturation, z=invGamma, w=distortAmount (ignored)
    vec4 p3;   // xyz=cdlPower, w=dither amplitude (LSBs)
    vec4 p4;
    vec4 p5;   // x = CPU-smoothed exposure (<=0: use the instantaneous estimate)
    vec4 p6;
    vec4 p7;   // zw = 1/display size when rendering below display resolution, else 0
} pc;

layout(location = 0) out vec4 outColor;
const vec3 LUM = vec3(0.2126, 0.7152, 0.0722);

void main()
{
    // nfs28 (tonemap_lean_v2): p5.z > 0.5 = the bare resample probe (nx_diag 111) -- fetch + write, no tone curve,
    // so the windows beside it price the curve against the resample. Uniform branch; 0 in normal frames.
    if (pc.p5.z > 0.5) {
        vec2 uvP = (pc.p7.z > 0.0) ? gl_FragCoord.xy * pc.p7.zw : gl_FragCoord.xy / vec2(textureSize(uResolved, 0));
        outColor = vec4(clamp(textureLod(uResolved, uvP, 0.0).rgb, 0.0, 1.0), 1.0);
        return;
    }
    float exposure = pc.p5.x;
    if (exposure <= 0.0) {
        vec3 avg = clamp(textureLod(uHDR, vec2(0.5), pc.p0.y).rgb, vec3(0.0), vec3(65504.0));
        float avgLum = max(dot(avg, LUM), 1e-4);
        exposure = clamp(pc.p0.z / (avgLum + pc.p0.w), pc.p1.x, pc.p1.y) * pc.p1.z;
    }
    // 10-09: p4.y > 0.5 = the scene and this target are 1:1 (the Switch display crop): read the texel itself. The
    // bilinear tap of an RGBA16F scene runs at half rate on Maxwell and the size query is a texture op of its own --
    // the composite's 0.6 ms (v284 probes: 0.65 -> 0.05 without its pixels, unchanged off the swapchain).
    vec3 c;
    if (pc.p4.y > 0.5)
        c = texelFetch(uResolved, ivec2(gl_FragCoord.xy), 0).rgb;
    else {
        vec2 uv = (pc.p7.z > 0.0) ? gl_FragCoord.xy * pc.p7.zw
                                  : gl_FragCoord.xy / vec2(textureSize(uResolved, 0));
        c = textureLod(uResolved, uv, 0.0).rgb;
    }
    c = max(c, vec3(0.0));
    c = (c == c) ? c : vec3(0.0);                       // NaN firewall (display only)
    c = min(c, vec3(65504.0)) * exposure;

    // nfs29 (tonemap_lean_v3): at the defaults the CDL power (2*(1-0.5)), the saturation and the gamma (1/(1*1)) are
    // exactly 1.0 -- two vec3 pow = ~12 transcendental ops per pixel for nothing. Uniform branches skip them; any
    // other knob value takes the full path. p4.x > 0.5 forces the full path (nx_diag 115, the A/B).
    const bool full = pc.p4.x > 0.5;
    c *= pc.p2.x;                                        // CDL slope
    if (full || pc.p3.x != 1.0 || pc.p3.y != 1.0 || pc.p3.z != 1.0)
        c = pow(max(c, vec3(0.0)), pc.p3.xyz);           // power
    if (full || pc.p2.y != 1.0) {
        float luma = dot(c, LUM);
        c = max(luma + pc.p2.y * (c - luma), vec3(0.0)); // saturation
    }

    float W = pc.p0.x;
    c = (c / (c + 1.0)) * ((W + 1.0) / W);               // Reinhard, white = W (normaliser is uniform)
    if (full || pc.p2.z != 1.0)
        c = pow(max(c, vec3(0.0)), vec3(pc.p2.z));       // gamma

    if (pc.p3.w > 0.0) {
        float dA = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
        float dB = fract(52.9829189 * fract(dot(gl_FragCoord.xy + 23.14069, vec2(0.06711056, 0.00583715))));
        c += vec3((dA - dB) * (pc.p3.w / 255.0));
    }
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
