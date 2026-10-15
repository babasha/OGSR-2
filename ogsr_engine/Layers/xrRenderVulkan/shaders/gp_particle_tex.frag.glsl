#version 450
#extension GL_EXT_nonuniform_qualifier : require

// GPU particle billboard fragment shader — textured (bindless) variant.
// Samples the effect's sprite texture from a bindless array indexed per
// particle by its program slot (fragLayer). Untextured particles (fragLayer < 0,
// e.g. the campfire test program) fall back to the procedural soft circle.
//
// Stage-0 volumetric light-probe (matches the CPU particle pass particle.frag):
// when the draw pushes strength > 0 (world-phase alpha smoke only — never the
// additive/HUD groups, and only when r_vol ran) the smoke is brightened by the
// LOCAL froxel in-scatter at its position, so it catches the sun shaft /
// flashlight cone / campfire glow instead of reading as a flat unlit sticker.

layout(set = 1, binding = 0) uniform sampler2D uTextures[];
layout(set = 2, binding = 0) uniform sampler3D uScatter;   // froxel in-scatter (rgb) + extinction (a)

layout(location = 0) in vec4      fragColor;
layout(location = 1) in vec2      fragUV;
layout(location = 2) flat in int  fragLayer;
layout(location = 3) in float     fragVolW;
layout(location = 4) in vec2      fragScreenUV;

layout(push_constant) uniform PC {
    mat4  viewProj;
    vec4  camRight;      // xyz + near
    vec4  camUp;         // xyz + logFN
    vec4  camPosStr;     // xyz + probe strength
    vec4  camDirClamp;   // xyz + radiance clamp
};

layout(location = 0) out vec4 outColor;

void main()
{
    if (fragLayer < 0) {
        // Procedural soft circle (no sprite texture for this program).
        vec2  uv_c  = fragUV - 0.5;
        float alpha = 1.0 - smoothstep(0.3, 0.5, length(uv_c));
        if (alpha < 0.01) discard;
        outColor = vec4(fragColor.rgb, fragColor.a * alpha);
    } else {
        vec4 tex = texture(uTextures[nonuniformEXT(fragLayer)], fragUV);
        vec4 c   = tex * fragColor;          // modulate sprite by per-particle colour/alpha
        if (c.a < 0.003) discard;
        outColor = c;
    }

    // Stage-0 volumetric light probe — additive, coverage-weighted, clamped so
    // smoke never blows to white next to a campfire / sun beam. strength == 0
    // for additive + HUD draws (self-emissive / different projection).
    float strength = camPosStr.w;
    if (strength > 0.0) {
        vec4 s   = texture(uScatter, vec3(fragScreenUV, clamp(fragVolW, 0.0, 1.0)));
        vec3 rad = s.rgb / max(s.a, 1e-3);   // density-independent local radiance
        rad = min(rad, vec3(camDirClamp.w));
        outColor.rgb += rad * (strength * outColor.a);
    }
}
