#version 450
#extension GL_GOOGLE_include_directive : require
// nfs46 (Switch): the half-res sun mask. One texel covers full-res pixels (2x..2x+1, 2y..2y+1); it takes the NEAREST of
// their prepass depths (foreground wins), reconstructs the world point and runs the cascade/far lookup once.
// Out: R = shadow (1 lit), G = how much a live map covers the point, B = the depth it was computed at (for the upsample),
// A = the flashlight's shadow x cookie (10-09, r_flash_mask: L.lamp_terrain.y), 1 otherwise.
#define ENV_SET 1
#include "light_ubo.glsl"
#include "shadow_common.glsl"   // spotShadowF (+ shadow_math: cascTap)
#include "nx_sun_shadow.glsl"
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(push_constant) uniform P { mat4 invViewProj; vec4 p; } pc;   // p.xy = 1 / full-res size
layout(location = 0) out vec4 outM;

// The flashlight term the R1 receivers multiply its light by (light_shade.glsl): what the LAMPSH variant computed per
// pixel -- its spot tile's shadow and the cookie -- once per 2x2 here. The cookie goes in as luminance: the beam texture
// is a grey pattern, and the mask has one free channel.
float torchTerm(vec3 wp)
{
    const int s = int(L.shadow_params.x);
    if (L.lamp_terrain.y < 0.5 || s < 0 || s >= 16) return 1.0;
    float t = 1.0;
    const int tile = spotTileOf(s);
    if (tile >= 0) t = spotShadowF(wp, L.lights[s].pos.w, tile);
    if (L.shadow_params.z > 0.5) {
        const vec4 cc = L.spot_vp * vec4(wp, 1.0);
        if (cc.w > 0.0) {
            vec2 cuv = (cc.xy / cc.w) * 0.5 + 0.5;
            cuv.y = 1.0 - cuv.y;
            t *= dot(textureLod(uSpotCookie, clamp(cuv, 0.0, 1.0), 0.0).rgb, vec3(0.2126, 0.7152, 0.0722));
        }
    }
    return t;
}

void main()
{
    const vec2  uvC = (floor(gl_FragCoord.xy) * 2.0 + 1.0) * pc.p.xy;   // the shared corner of the 2x2 full-res block
    vec4        d4  = textureGather(uDepth, uvC, 0);
    // r_hud_prepass: the hands/weapon sit in the prepass at the HUD near range [0, 0.02] -- no world point there, and
    // letting them win the min would hand the world pixels along their outline a mask computed for the gun.
    d4 = mix(d4, vec4(1.0), lessThan(d4, vec4(0.02)));
    const float z   = min(min(d4.x, d4.y), min(d4.z, d4.w));
    if (z >= 0.99999) { outM = vec4(1.0, 0.0, 1.0, 1.0); return; }   // sky
    const vec4 clip = vec4(uvC.x * 2.0 - 1.0, 1.0 - 2.0 * uvC.y, z, 1.0);
    const vec4 w    = pc.invViewProj * clip;
    float cov;
    const float s = nxSunShadowCov(w.xyz / w.w, cov);
    outM = vec4(s, cov, z, torchTerm(w.xyz / w.w));
}
