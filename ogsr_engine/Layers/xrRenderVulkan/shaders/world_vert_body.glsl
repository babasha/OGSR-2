// r_world_bindless twins (-DWORLD_BINDLESS): the material index rides location 8 (flat), set 0 = the bindless set.
// First: the include carries an #extension, which must precede every declaration.
#ifdef WORLD_BINDLESS
#define WORLD_BINDLESS_VS
#include "world_bindless.glsl"
#else
#define WORLD_DETAIL_SCALE pc.detailScale
#endif


// World pass VS, BOTH static-lighting variants (-DWORLD_VLIT picks vert-lit).
// Everything except where the baked lighting comes FROM is identical, so the
// transform (instanced / dynamic-prop model matrix / sub-pixel UV) is written once.
//
//   lmap — X-Ray level statics with a baked LIGHTMAP (tcOffset == 24 sub-layout).
//          TC1 at offset 28 is the per-mesh-region lightmap UV (SHORT2, scale
//          1/32768 -> unit range). The lightmap RGBA (set 0, binding 2) carries
//          baked hemi/sun/bounce colour from the level compiler.
//   vlit — statics whose lighting is baked per-vertex into a D3DCOLOR at offset 24
//          (tcOffset == 28 sub-layout). COLOR.bgr = baked RGB lighting (point
//          lights + bounce); COLOR.a = sun mask. The baked SKY ACCESS (hemi) is NOT
//          in that colour — it rides in the alpha of the packed NORMAL:
//          `float4 Nh : NORMAL; // (nx,ny,nz,hemi occlusion)` (X-Ray
//          common_iostructs.h, v_static_color), which deffer_base_flat.ps reads
//          back as `h = I.position.w` for exactly this geometry.
//
// NOTE: snow VOLUME is NOT applied here. Vertex displacement on a building (mixed
// normals: roof up, walls sideways) lifts the roof off the walls -> it detaches /
// levitates. Snow volume on statics needs a separate snow-shell layer; roofs get
// snow via the fragment (whiten + normal-smoothing) only.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV_short_raw;     // base UV  (SHORT2 SSCALED)
#ifndef XR_SHORT_ATTR_DECLARED
#define XR_SHORT_ATTR_DECLARED
layout(constant_id = 200) const float XR_SHORT_ATTR = 1.0; // 32767 where SSCALED is missing and SHORT2 comes as SNORM (vk_gfx_pipeline.cpp)
#endif
#define inUV_short (inUV_short_raw * XR_SHORT_ATTR)
layout(location = 2) in vec4 inTangent;      // .a = du (sub-pixel U fraction)
layout(location = 3) in vec4 inBinormal;     // .a = dv (sub-pixel V fraction)
#ifdef WORLD_VLIT
layout(location = 4) in vec4 inColor;        // D3DCOLOR @ 24 (BGRA in memory)
#else
layout(location = 4) in vec2 inLmapUV_short_raw; // lightmap UV (SHORT2 SSCALED)
#ifndef XR_SHORT_ATTR_DECLARED
#define XR_SHORT_ATTR_DECLARED
layout(constant_id = 200) const float XR_SHORT_ATTR = 1.0; // 32767 where SSCALED is missing and SHORT2 comes as SNORM (vk_gfx_pipeline.cpp)
#endif
#define inLmapUV_short (inLmapUV_short_raw * XR_SHORT_ATTR)
#endif
layout(location = 5) in vec4 inNormal;       // D3DCOLOR @ 12 (BGRA in memory)

layout(push_constant) uniform PushConstants {
    mat4  mvp;
    vec2  uvScale;
    float alphaRef;
    float detailScale;
    // dynHemi < -0.5 = DYNAMIC visual (spawned prop: urn/bed/item). Real hemi is
    // -dynHemi-1 (frag decodes); the model matrix rows follow (pushed by Flush into
    // the tess region — dynamics never tessellate). Statics stay on the plain path.
    float dynHemi;
    float dmi0, dmi1, dmi2;   // model row i (X basis, carries uniform scale)
    float dmj0, dmj1, dmj2;   // model row j (Y basis)
    float dmc0, dmc1, dmc2;   // model translation
} pc;

// Slot assignment differs per variant — must stay in lockstep with the matching
// #ifdef block in world_frag_body.glsl.
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec2 vDetailUV;
#ifdef WORLD_VLIT
layout(location = 2) out vec3  vBakedColor;   // RGB lighting (BGR->RGB swizzle)
layout(location = 3) out float vSunMask;
layout(location = 4) out vec3  vWorldPos;     // statics are world-space (identity model)
layout(location = 5) out vec3  vNormal;       // world-space normal (dynamic lights)
layout(location = 7) out float vBakedHemi;    // NORMAL.a = baked sky access (see header)
#else
layout(location = 2) out vec2 vLmapUV;
layout(location = 3) out vec3 vWorldPos;   // level statics are world-space (identity model)
layout(location = 4) out vec3 vNormal;     // world-space normal (dynamic lights)
#endif

#ifdef CLUSTER_FADE
#ifdef WORLD_VLIT
layout(location = 6) flat out uint vFadeBits;   // cull-packed LOD fades (via firstInstance -> gl_InstanceIndex)
#else
layout(location = 5) flat out uint vFadeBits;   // cull-packed LOD fades (via firstInstance -> gl_InstanceIndex)
#endif
#endif

#ifdef INSTANCED
// Host-driven instanced scene (vk_instance_gpu): the model matrix arrives as four
// INSTANCE-rate vertex attributes (binding 1) instead of push constants, so ONE
// indirect draw covers every instance of a mesh. The X-Ray Fmatrix is row-major,
// so its rows map straight onto i/j/k/c and the transform stays `v * M`.
// pc.mvp is the plain viewProj on this path — the model transform happens here.
layout(location = 6) in vec4 aXfI;   // model row i (X basis)
layout(location = 7) in vec4 aXfJ;   // model row j (Y basis)
layout(location = 8) in vec4 aXfK;   // model row k (Z basis)
layout(location = 9) in vec4 aXfC;   // model translation (row c)
#endif

void main()
{
#ifdef CLUSTER_FADE
    vFadeBits = uint(gl_InstanceIndex);
#endif
    vec3 pos = inPos;
    // D3DCOLOR memory order is BGRA -> real (x,y,z) = .bgr; unpack [0,1] -> [-1,1].
    vec3 nrm = inNormal.bgr * 2.0 - 1.0;
    // Dynamic props reuse this static pipeline but carry a real model matrix —
    // without this transform every world-space consumer (fog distance, cascade sun
    // shadow, dynamic lights, wetness/snow) read MODEL-space coords: props got the
    // fog of the LEVEL ORIGIN (washed-out "glow" that ignores the surroundings) and
    // rotated objects were sun-lit from the wrong side (unrotated normals).
#ifdef INSTANCED
    // Unconditional: every draw on this pipeline is instanced. All FOUR rows are
    // real here, where the push-constant path below only carried i/j and had to
    // rebuild k as i×j — so non-uniform scale and mirroring survive intact.
    pos = pos.x * aXfI.xyz + pos.y * aXfJ.xyz + pos.z * aXfK.xyz + aXfC.xyz;
    nrm = normalize(nrm.x * aXfI.xyz + nrm.y * aXfJ.xyz + nrm.z * aXfK.xyz);
    gl_Position = pc.mvp * vec4(pos, 1.0);   // pc.mvp == viewProj
#else
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    if (pc.dynHemi < -0.5) {
        vec3 mi = vec3(pc.dmi0, pc.dmi1, pc.dmi2);
        vec3 mj = vec3(pc.dmj0, pc.dmj1, pc.dmj2);
        vec3 mk = cross(mi, mj) / max(length(mi), 1e-6);   // det=+1 rotation ⇒ k = i×j (scale folded)
        pos = pos.x * mi + pos.y * mj + pos.z * mk + vec3(pc.dmc0, pc.dmc1, pc.dmc2);
        nrm = normalize(nrm.x * mi + nrm.y * mj + nrm.z * mk);
    }
#endif
    vWorldPos   = pos;
    vNormal     = nrm;

    // Sub-pixel UV: 8 extra bits of fractional du/dv from packed tangent/binormal alphas.
    vec2 uv     = inUV_short + vec2(inTangent.a, inBinormal.a);
    vUV         = uv * pc.uvScale;
#ifdef WORLD_BINDLESS
    vMatId      = worldEntryMat[uint(gl_InstanceIndex) & 0xFFFFFu];   // the cull's entry id -> this draw's material
#endif
    vDetailUV   = vUV * WORLD_DETAIL_SCALE;

#ifdef WORLD_VLIT
    // D3DCOLOR memory order is BGRA; swizzle .bgr to recover real RGB.
    vBakedColor = inColor.bgr;
    vSunMask    = inColor.a;
    vBakedHemi  = inNormal.a;   // hemi rides in the normal's alpha, not in the colour
#else
    // Lightmap UV scale is 1/32768 (range +-1) - different from base UV's 1/1024.
    vLmapUV     = inLmapUV_short * (1.0 / 32768.0);
#endif
    // -nx_bisect2 "(S: off-screen verts)" (Switch, vk_world_gpu DrawColor pushes dynHemi -10; dynamics use [-2,-1]):
    // the whole VS still runs, every triangle lands outside the clip volume -> no raster, no FS, no ROP.
    if (pc.dynHemi < -5.0) gl_Position = vec4(2.0, 2.0, 0.5, 1.0);
}
