#version 450
// xrRenderVulkan — NORMAL G-buffer caster for STATIC level geometry.
//
// Hands GTAO the SAME vertex normal the colour pass shades with
// (world_vert_body.glsl: `inNormal.bgr * 2 - 1`, then the dynamic-prop model
// rotation), so the receiver's geomN and the GTAO input normal agree exactly.
// That agreement is the whole point: the depth-derivative normal this replaces
// sits 15-25 degrees off geomN at grazing angles, which is what forced the
// r_ssao_bias grazing fade and defeated every bent-normal gate (see the полосы
// saga in the ssao/gtao notes).
//
// Position @0 and the packed normal @12 are at FIXED offsets in EVERY level
// static vertex layout — lmap and vert-lit alike (vk_pipeline_cache.cpp
// BuildVertexInputForStride) — so two attributes cover both sub-layouts and the
// pipeline only has to be keyed by stride, exactly like the depth caster.
//
// NORMAL_AT twin: + the base UV so the fragment can punch out transparent
// texels with the SAME test the depth prepass used (shadow_depth_at) — a grate
// must not stamp its quad's normal onto whatever is behind it.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inNormal;     // D3DCOLOR @ 12 (BGRA in memory)
#ifdef NORMAL_AT
layout(location = 2) in vec2 inUV_short_raw;   // SHORT2 SSCALED @ tcOffset (24/28)
#ifndef XR_SHORT_ATTR_DECLARED
#define XR_SHORT_ATTR_DECLARED
layout(constant_id = 200) const float XR_SHORT_ATTR = 1.0; // 32767 where SSCALED is missing and SHORT2 comes as SNORM (vk_gfx_pipeline.cpp)
#endif
#define inUV_short (inUV_short_raw * XR_SHORT_ATTR)
#endif

layout(push_constant) uniform PC {
    mat4 mvp;    // objectXform · viewProj — the model TRANSLATION/scale is folded in here
    vec4 mi;     // .xyz = model row i (X basis); .w = uvScale.x
    vec4 mj;     // .xyz = model row j (Y basis); .w = uvScale.y
    vec4 misc;   // .x = alphaRef; .y > 0.5 = the model rows are real (dynamic prop)
} pc;

layout(location = 0) out vec3 vNormal;
#ifdef NORMAL_AT
layout(location = 1) out vec2 vUV;
#endif
// The baked sky access (hemi) rides in the SAME attribute's alpha — the byte
// this pass has fetched all along and threw away. Exported so the gather can
// bound its distant-ambient fallback by OFFLINE enclosure instead of guessing
// it from radiance-cache cells (see probeFallbackCapped in gather.comp).
layout(location = 2) out float vHemi;

void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);

    // D3DCOLOR memory order is BGRA -> real (x,y,z) = .bgr; unpack [0,1] -> [-1,1].
    vec3 nrm = inNormal.bgr * 2.0 - 1.0;

    // Dynamic props (spawned urns/beds/crates) ride this same static geometry
    // path with a real model matrix. Mirror world_vert_body's non-instanced
    // branch EXACTLY — k rebuilt as i×j with the uniform scale divided out — so
    // the normal GTAO reads is the one the colour pass interpolates. Level
    // statics carry an identity xform and skip the whole block.
    if (pc.misc.y > 0.5) {
        vec3 mi = pc.mi.xyz, mj = pc.mj.xyz;
        vec3 mk = cross(mi, mj) / max(length(mi), 1e-6);
        nrm = nrm.x * mi + nrm.y * mj + nrm.z * mk;
    }
    vNormal = nrm;
    vHemi   = inNormal.a;   // baked hemi — same packing world_vert_body reads

#ifdef NORMAL_AT
    // No sub-pixel du/dv here — shadow_depth_at.vert does not use it either, and
    // matching IT is what keeps the punched-out coverage identical to the depth
    // the LEQUAL test runs against.
    vUV = inUV_short * vec2(pc.mi.w, pc.mj.w);
#endif
}
