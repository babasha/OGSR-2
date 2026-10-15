#ifndef RT_SHADE_GLSL
#define RT_SHADE_GLSL
// xrRenderVulkan — SHADING A RAY-QUERY HIT against the camera-local BVH.
//
// RTAO only ever asked the BVH "is anything in the way", which needs nothing but
// the acceleration structure. Everything past AO — indirect light, sun shadows
// with a coloured bounce, reflections — needs the hit to be SHADEABLE: a normal,
// an albedo, and the direct light arriving there. This file is that step, kept
// separate from any pass so the probe update and the debug view use the SAME
// hit-shading code (two copies would drift, and the drift would look like a bug
// in the probes).
//
// WHERE THE ATTRIBUTES COME FROM
// ------------------------------
// The BLAS is built from a flat triangle soup with NO index buffer (see
// vk_rt_scene.cpp), so `primitiveIndex * 3` is the first of the three vertices —
// no indirection table needed. That gives the GEOMETRIC normal by cross product.
//
// Albedo is the interesting one, and it depends on WHICH WORLD the soup was cut
// from (r_rt_src). Both write the same per-triangle attribute block — four uints:
// a material slot and the three vertices' packed texcoords — so this file has one
// code path and the difference lives in the table it looks the slot up in:
//
//   COLLISION (level.cform) — no UVs and no materials in the render sense, but
//     every triangle carries a 14-bit GAME MATERIAL index (CDB::TRI::material,
//     the field next to the suppress_shadows bit the soup filter already reads).
//     The CPU turns the material library into a table of linear albedos once per
//     session, so a hit resolves to "concrete grey" or "grass green" — one albedo
//     per material CLASS, not per texel, and the texcoords are zero.
//
//   RENDER GEOMETRY (vk_rt_world) — the material slot indexes the DIFFUSE of the
//     material the surface is actually drawn with, and the texcoords are real, so
//     a hit resolves to a TEXEL. The table entry's w carries the texture slot;
//     w < 0 means "no texture behind this material, use the rgb" and is what the
//     collision path writes everywhere.
//
// ⚠ std430 LAYOUT: the vertex soup is TIGHTLY packed Fvector (12 bytes), and a
// `vec3` array in std430 has a 16-byte stride. Declaring it as vec3[] would read
// every vertex from the wrong offset and produce a plausible-looking, wrong
// mesh. It is declared as a float[] and indexed by hand for exactly that reason.
//
// Requires: GL_EXT_ray_query, #version 460, and the includer must define
// RT_B_TLAS / RT_B_VERTS / RT_B_MATS / RT_B_ALBEDO (binding numbers) before the
// #include, plus RT_SET if the set is not 0.

#ifndef RT_SET
#define RT_SET 0
#endif

layout(set = RT_SET, binding = RT_B_TLAS) uniform accelerationStructureEXT rtTLAS;

// 3 floats per vertex, 3 vertices per triangle, no index buffer.
layout(std430, set = RT_SET, binding = RT_B_VERTS)  readonly buffer RtVerts  { float rtVertF[]; };
// FOUR uints per TRIANGLE: [0] material slot, [1..3] the three vertices'
// UVs as half2 — already scaled by the gather (zero on the cform path, unread there).
layout(std430, set = RT_SET, binding = RT_B_MATS)   readonly buffer RtMats   { uint  rtAttr[]; };
// ⭐⭐⭐GI ALBEDO BOOST (r_rt_albedo). Set by the caller from ddgi.casc.w before it
// traces; 1.0 is the untouched texture and the A/B baseline.
//
// WHY A RENDERER SHOULD EVER LIE ABOUT REFLECTANCE. The multi-bounce equilibrium of
// a room is its aperture influx times 1/(1 - albedo), so the number matters far more
// indoors than the direct term ever suggests: 0.3 amplifies by 1.43, 0.55 by 2.2.
// Measured on Dark Valley 22-08, a room whose wallpaper reads ~0.3 stayed dim no
// matter how correct the aperture integral got, because there was nothing in it to
// bounce off.
//
// And 0.3 is not the wallpaper's real reflectance — it is what the ARTIST authored
// for a renderer that had no GI at all. In that pipeline light was a constant added
// on top, so diffuse maps were painted dark on purpose to keep the sum from washing
// out. Feeding those maps to a physical integrator under-reflects every interior in
// the game by roughly the same factor. This is the one honest place to correct it,
// and it stays a knob rather than a constant because the right value is a property
// of the ART, not of the renderer.
//
// ⚠ GI ONLY. The raster path samples the same textures and must NOT see this — the
// direct term is already calibrated against them.
// ⚠ The 0.9 cap below stays: a surface returning more than it received makes the
// multi-bounce feedback ring, and this knob is exactly how someone would get there.
float rtAlbedoBoost = 1.0;

// Material slot -> (fallback linear albedo rgb, texture slot in w; w < 0 = no texture).
layout(std430, set = RT_SET, binding = RT_B_ALBEDO) readonly buffer RtAlbedo { vec4  rtAlbedoTab[]; };

#ifdef RT_TEX_SET
// The diffuse of every material in the static world set, as one descriptor array
// (vk_rt_world). Indexed per invocation — a ray from one probe and the ray next to
// it land on different materials — so the index is NOT dynamically uniform, and
// the includer must declare GL_EXT_nonuniform_qualifier at the top of its file
// (an #extension here would land after the first tokens of the shader).
// ⚠ RT_TEX_COUNT must equal VK::RTWorld::kMaxMaterials — the array size is baked
// into the set layout, and a mismatch makes the pipeline layout incompatible.
#ifndef RT_TEX_COUNT
#define RT_TEX_COUNT 4096
#endif
layout(set = RT_TEX_SET, binding = 0) uniform sampler2D rtDiffuse[RT_TEX_COUNT];

// The gather already turned the source's quantised texcoord into a real UV and
// packed it as a half2 — level statics quantise at 1/1024 and trees at 1/2048,
// and this side has no business knowing which it is looking at.
vec2 rtUnpackUV(uint p) { return unpackHalf2x16(p); }
#endif

struct RTHit {
    bool  hit;
    vec3  pos;      // world-space hit point
    vec3  nrm;      // geometric normal, flipped to face the incoming ray
    vec3  albedo;   // linear diffuse reflectance
    float dist;     // t along the ray (= tmax on a miss)
    // The face was hit from BEHIND, i.e. the winding says the ray came out of the
    // solid side. Only probe relocation reads this, and only as a corroborating
    // signal, because the value is only as trustworthy as the source geometry:
    // collision winding (r_rt_src 0) carries no information at all, which is why
    // the normal above is oriented against the ray rather than by the winding.
    // Render geometry (r_rt_src 1) is wound for raster backface culling and does
    // carry it. r_ddgi_reloc_back decides whether it is believed, and the
    // relocation self-audit prints the measured rate so the answer is a number
    // rather than an assumption — a rate near 50% means "no information".
    bool  back;
    // Index of the triangle hit, in the flat soup both structures were built over.
    // Probe relocation compares it against the opaque/cutout boundary
    // (VK::RTWorld::OpaqueTris, shipped in ddgi.reloc.w) to tell a WALL from a
    // LEAF CARD — 0 on a miss.
    uint  prim;
};

vec3 rtVertex(uint i)
{
    return vec3(rtVertF[i * 3u + 0u], rtVertF[i * 3u + 1u], rtVertF[i * 3u + 2u]);
}

// Barycentric UV of one triangle of the soup.
vec2 rtUVAt(uint tri, vec2 bary)
{
    return (1.0 - bary.x - bary.y) * rtUnpackUV(rtAttr[tri * 4u + 1u])
         + bary.x                  * rtUnpackUV(rtAttr[tri * 4u + 2u])
         + bary.y                  * rtUnpackUV(rtAttr[tri * 4u + 3u]);
}

#ifdef RT_TEX_SET
// ---------------------------------------------------------------------------
//  CUTOUTS — the alpha test, which is what a forest is made of.
//
//  A leaf card is a rectangle whose texture is transparent nearly everywhere.
//  Treated as solid it blocks the sun over its whole quad, and a crown becomes a
//  black box: the difference between a wood and a wall. So the RT world is built
//  as TWO structures (see RTWorld::OpaqueTris) and only the cutout one asks this.
//
//  ⚠ THE MIP MATTERS MORE HERE THAN FOR COLOUR. Coarse mips of a leaf texture
//  average their alpha toward the coverage of the card — perhaps 0.3 — and every
//  texel then fails a 0.78 reference, dissolving the crown completely. The albedo
//  fetch WANTS a coarse mip (it is after an average); this one must stay fine.
// ---------------------------------------------------------------------------
#ifdef RT_ATEST_DYN
// ---------------------------------------------------------------------------
//  PRICING THE ALPHA TEST (r_gather_atest) — a MEASUREMENT harness, compiled
//  only where the includer asks for it, so the probe pass and the world cache
//  keep their .spv byte-identical until a mode has earned its way in.
//
//  ⭐ WHAT IT IS FOR. `-foliage` in the ray profiler removes the cutout
//  structure ENTIRELY, so its -0.229 ms lumps together two different things:
//  TRAVERSING a million leaf cards, and ASKING A TEXTURE at each of them. Only
//  the second one is what opacity micromaps (or any software stand-in) would
//  remove, so building either without splitting the number first would be
//  buying a term whose size nobody has measured. These modes are that split.
//
//  x = mode  0 the exact per-texel test (today, bit-identical)
//            1 accept every candidate — cutouts turn SOLID. The floor of what
//              any alpha-test work could ever cost, and an optimistic one:
//              rays also terminate at the first card instead of passing through.
//            2 accept with a fixed probability, no texture and no UV fetch at
//              all. The honest "no fetch" number: with y set to the cards'
//              measured coverage the traversal statistics stay comparable.
//            3 coverage as a PROBABILITY, read from a coarse — hence
//              cache-resident — mip. Not just a probe: an alpha test that
//              samples instead of thresholding is unbiased at ANY mip, which is
//              exactly the property the fine-mip requirement below costs us.
//  y = the fixed coverage of mode 2.
//  z = minimum mip for the exact test (mode 0) / for mode 3's coverage read.
//  w = distance past which the cheap mode takes over; 0 = everywhere.
vec4 rtAtest = vec4(0.0, 0.35, 0.0, 0.0);

// Deterministic in the HIT POINT, deliberately: a hash that also varied per
// frame would trade the texture fetch for temporal flicker on every leaf edge,
// and the point of a stochastic test here is that the neighbouring rays of the
// same surface average it out in SPACE, where the resolve filter already works.
float rtAtestHash(uint tri, vec2 b)
{
    uvec3 p = uvec3(tri, uint(b.x * 4096.0), uint(b.y * 4096.0));
    p = p * 1664525u + 1013904223u;
    p.x += p.y * p.z;  p.y += p.z * p.x;  p.z += p.x * p.y;
    p ^= p >> 16u;
    p.x += p.y * p.z;
    return float(p.x & 0x00FFFFFFu) * (1.0 / 16777216.0);
}
#endif

bool rtAlphaAccept(uint tri, vec2 bary, float t)
{
    const uint slot = rtAttr[tri * 4u + 0u];
    const vec4 tab  = rtAlbedoTab[min(slot, uint(rtAlbedoTab.length() - 1))];
    if (tab.w < 0.0 || tab.x < 0.0) return true;   // no texture, or not a cutout material
#ifdef RT_ATEST_DYN
    if (rtAtest.x > 0.0 && t >= rtAtest.w) {
        if (rtAtest.x < 1.5) return true;                                  // 1: solid
        if (rtAtest.x < 2.5) return rtAtestHash(tri, bary) < rtAtest.y;    // 2: no texture at all
        // 3: the mip is coarse ON PURPOSE. Thresholding a coarse mip dissolves
        // the crown (see above); sampling one does not, because the mean alpha
        // IS the coverage the finer levels would have thresholded to.
        return rtAtestHash(tri, bary)
             < textureLod(rtDiffuse[nonuniformEXT(int(tab.w))], rtUVAt(tri, bary), max(rtAtest.z, 3.0)).a;
    }
    const float lod = clamp(log2(1.0 + t * 0.25), rtAtest.z, 3.0 + rtAtest.z);
#else
    const float lod = clamp(log2(1.0 + t * 0.25), 0.0, 3.0);
#endif
    return textureLod(rtDiffuse[nonuniformEXT(int(tab.w))], rtUVAt(tri, bary), lod).a >= tab.x;
}
#endif

// Closest hit with attributes. `d` must be normalised.
//
// `mask` is ANDed with each TLAS instance's own mask, and vk_rt_scene tags the two
// structures it builds 0x01 (opaque world) and 0x02 (alpha-tested cutouts, i.e.
// essentially the forest). A consumer that cannot afford a million leaf cards can
// therefore drop them without a second BVH — 0xFF, the default, keeps everything
// and is what every caller before the final gather used.
RTHit rtTraceClosestMasked(vec3 o, vec3 d, float tmin, float tmax, uint mask)
{
    RTHit h;
    h.hit = false; h.dist = tmax; h.pos = o + d * tmax; h.nrm = -d; h.albedo = vec3(0.0);
    h.back = false; h.prim = 0u;

    rayQueryEXT rq;
    // NOT RayFlagsOpaque: that would force-commit the cutout structure's hits too,
    // and every leaf card would be a solid quad. Opaque geometry still commits
    // inside the traversal — only the cutout instance reaches the loop body.
    rayQueryInitializeEXT(rq, rtTLAS, gl_RayFlagsNoneEXT, mask, o, tmin, d, tmax);
    while (rayQueryProceedEXT(rq)) {
#ifdef RT_TEX_SET
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            const uint ctri = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false))
                            + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
            if (rtAlphaAccept(ctri, rayQueryGetIntersectionBarycentricsEXT(rq, false),
                              rayQueryGetIntersectionTEXT(rq, false)))
                rayQueryConfirmIntersectionEXT(rq);
        }
#else
        rayQueryConfirmIntersectionEXT(rq);
#endif
    }
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return h;

    // The instance's custom index is the FIRST TRIANGLE of the structure it came
    // from, so this lands in the one flat soup both of them were built over.
    const uint  prim = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true))
                     + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
    const float t    = rayQueryGetIntersectionTEXT(rq, true);
    const vec2  bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);

    const vec3 v0 = rtVertex(prim * 3u + 0u);
    const vec3 v1 = rtVertex(prim * 3u + 1u);
    const vec3 v2 = rtVertex(prim * 3u + 2u);

    // The instance is built TRIANGLE_FACING_CULL_DISABLE because collision winding
    // is not dependable, so the sign of the cross product carries no information —
    // orient against the ray instead.
    vec3  n = cross(v1 - v0, v2 - v0);
    float l = length(n);
    n = (l > 1e-12) ? (n / l) : -d;
    h.back = dot(n, d) > 0.0;   // before the flip: this is the only place the winding survives
    if (h.back) n = -n;

    h.hit    = true;
    h.dist   = t;
    h.pos    = o + d * t;
    h.nrm    = n;
    h.prim   = prim;

    const uint slot = rtAttr[prim * 4u + 0u];
    const vec4 tab  = rtAlbedoTab[min(slot, uint(rtAlbedoTab.length() - 1))];
    h.albedo = min(tab.rgb * rtAlbedoBoost, vec3(0.9));
#ifdef RT_TEX_SET
    if (tab.w >= 0.0) {
        const vec2 uv = rtUVAt(prim, bary);
        // ⭐A COARSE MIP IS THE POINT, NOT A COMPROMISE. Indirect light wants the
        // AVERAGE reflectance of the patch a ray landed on, and there are no
        // derivatives here to pick a mip from — so the level rides the hit
        // distance. It also keeps far hits inside whatever mips the streamer has
        // resident, which the finest levels routinely are not.
        const float lod = clamp(log2(1.0 + t * 0.5), 2.0, 8.0);
        const vec3 c = textureLod(rtDiffuse[nonuniformEXT(int(tab.w))], uv, lod).rgb;
        // ⚠ NO sRGB->LINEAR HERE, AND THAT IS DELIBERATE. r_linear_color defaults
        // to 0, so the views are UNORM and the raster's own lighting multiplies
        // this very number by the sun colour. A bounce has to agree with the light
        // it is a bounce OF — converting only here would make every indirect term
        // about 3x darker than the direct one it comes from, which reads as "the
        // GI does nothing" rather than as a colour-space bug. When the linear
        // colour arc lands, this is the line that changes.
        // Cap at 0.9: a surface that returns more than it received makes the
        // multi-bounce feedback ring.
        h.albedo = min(c * rtAlbedoBoost, vec3(0.9));
    }
#endif
    return h;
}

RTHit rtTraceClosest(vec3 o, vec3 d, float tmin, float tmax)
{
    return rtTraceClosestMasked(o, d, tmin, tmax, 0xFFu);
}

// Any-hit visibility. Cheaper than rtTraceClosest — it stops at the first hit and
// reads no attributes — so this is what shadow and occlusion rays use.
bool rtOccluded(vec3 o, vec3 d, float tmin, float tmax)
{
    rayQueryEXT rq;
    // Cutouts have to be tested here TOO, and this is where it costs: a shadow ray
    // through a crown asks the leaf texture whether it was actually stopped. Force
    // opacity instead and the forest floor goes uniformly black — the raster's own
    // tree shadows are alpha-tested for the same reason.
    rayQueryInitializeEXT(rq, rtTLAS, gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF, o, tmin, d, tmax);
    while (rayQueryProceedEXT(rq)) {
#ifdef RT_TEX_SET
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            const uint ctri = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false))
                            + uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false));
            if (rtAlphaAccept(ctri, rayQueryGetIntersectionBarycentricsEXT(rq, false),
                              rayQueryGetIntersectionTEXT(rq, false)))
                rayQueryConfirmIntersectionEXT(rq);
        }
#else
        rayQueryConfirmIntersectionEXT(rq);
#endif
    }
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// ---------------------------------------------------------------------------
//  IS THERE A CEILING ABOVE THIS POINT?  (r_gi_indoor)
// ---------------------------------------------------------------------------
// The half r_rt_albedo was missing: not HOW MUCH to correct the art by, but WHERE.
// This game diffuse maps were authored for an engine with no GI, where a constant
// ambient was added on top, so they are deliberately dark (~0.25 where a real
// plaster wall is 0.5-0.6). An enclosure settles at inflow x 1/(1-albedo), so at
// 0.25 the light entering a window survives barely one bounce - the windowsill is
// lit and a metre further in there is nothing. Raising the number globally fixes
// the room and blows out the street, which is why r_rt_albedo has shipped neutral.
//
// One opaque ray, straight up, a few metres. A room has a ceiling. Open ground does
// not. A forest canopy sits far above the reach, so the woods stay bit-identical -
// which is the whole requirement: correct the house, do not touch the street.
//
// ⭐ONE RAY, NOT A CONE, AND THAT IS THE DESIGN. Every caller is already inside a
// loop of rays (a probe fires 64, a cache cell 8-32, the gather one per pixel) and
// each calls this with its OWN jitter, so the binary answers average into the true
// blocked fraction INSIDE the integral the caller was computing anyway. A cone per
// call would pay six times over for an estimate the loop gives away.
// ⚠MASK 0x01 = THE OPAQUE STRUCTURE ONLY, and RayFlagsOpaque so no alpha test runs:
// a leaf card is not a ceiling, and asking the leaf texture would turn the cheapest
// ray in the frame into one of the most expensive (see rtOccluded on why cutouts
// cost). Grass and fences are excluded for the same reason.
float rtIndoorUp(vec3 P, float reach, vec2 jitter)
{
    if (reach <= 0.0) return 0.0;
    // Up, tilted by up to ~24 degrees: a probe sitting directly under a beam would
    // read "indoors" from a plumb line alone, and one in the open under a wire would
    // too. The cap is what the loop samples.
    const float ang = 6.2831853 * jitter.x;
    const float rad = 0.45 * sqrt(clamp(jitter.y, 0.0, 1.0));
    const vec3  d   = normalize(vec3(rad * cos(ang), 1.0, rad * sin(ang)));

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, rtTLAS,
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
                          0x01u, P, 0.05, d, reach);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT
         ? 1.0 : 0.0;
}

// Outgoing radiance at a hit: sun through a shadow ray + a crude sky term.
//
// ⭐NO 1/PI. The engine's direct lighting is `albedo * sun_color * NdL` with no
// physical normalisation, and a probe that integrated a DIFFERENT convention
// would come out consistently darker than the surfaces around it. Matching the
// engine matters more here than matching radiometry.
//
// `skyAtN` is the sky radiance ALREADY EVALUATED for this hit's normal. It is a
// parameter rather than a constant times a lobe because the two callers have very
// different skies available: the debug preview carries one env colour in a push
// block, while the probe trace can evaluate the real SH9 sky in its own set — and
// the probe's answer has to agree with the analytic sky fill it replaces, or the
// swap reads as an exposure change instead of as GI.
vec3 rtShadeHitSky(RTHit h, vec3 toSun, vec3 sunColor, vec3 skyAtN, float shadowRange)
{
    const float ndl = max(0.0, dot(h.nrm, toSun));
    float vis = 0.0;
    if (ndl > 0.0) {
        // Offset along the normal: the BVH is collision geometry, so the hit point
        // is only approximately on the rendered surface (same reason RTAO carries
        // r_rtao_bias), and a shadow ray starting inside a face self-shadows.
        vis = rtOccluded(h.pos + h.nrm * 0.02, toSun, 0.01, shadowRange) ? 0.0 : 1.0;
    }
    return h.albedo * (sunColor * (ndl * vis) + skyAtN);
}

// Flat-sky convenience form: one colour split over a crude hemisphere lobe. What
// the preview uses, and what the probes used before they could reach the SH9 sky.
vec3 rtShadeHit(RTHit h, vec3 toSun, vec3 sunColor, vec3 skyColor, float shadowRange)
{
    if (!h.hit) return skyColor;
    const float skyAmt = 0.5 + 0.5 * h.nrm.y;   // up sees the sky, down sees the ground
    return rtShadeHitSky(h, toSun, sunColor, skyColor * skyAmt, shadowRange);
}

#ifdef RT_LIGHTS
// ---------------------------------------------------------------------------
//  DYNAMIC LIGHTS IN THE RT WORLD
//
//  Until this existed the probes knew exactly two light sources: the sun and the
//  sky. A torch, a campfire, an anomaly, a muzzle flash — none of them existed for
//  the field, so nothing they lit could ever bounce. In a game played at night with
//  a flashlight that is most of the interesting light in the frame.
//
//  ⚠ WORLD SPACE, NOT THE CLUSTER GRID. The clustered binning the raster uses is
//  tied to the view frustum, and a probe ray leaves the frustum immediately — that
//  is exactly why idTech 8 built a world-space cascaded light grid instead. We do
//  not need one: the engine collects at most a few dozen lights per frame (nearest
//  first around the camera), so a flat loop over that list beats any structure
//  built to accelerate it. Their grid exists because they bin 4000 lights.
//
//  ⭐ A SHADOW RAY REPLACES THE SHADOW MAP. The raster path has an atlas tile or a
//  cube per pooled light and nothing for the rest; here every light gets exact
//  visibility for the price of one short any-hit ray, bounded by the distance to
//  the light itself. That is the one place this path is strictly BETTER than the
//  raster it feeds.
//
//  Requires RT_B_LIGHTS (binding) and light_atten.glsl for the shared falloff.
// ---------------------------------------------------------------------------
layout(std430, set = RT_SET, binding = RT_B_LIGHTS) readonly buffer RtLights {
    vec4 rtLight[];   // 3 vec4 per light: pos+range, colour+isSpot, dir+cos(half)
};

vec3 rtShadeLights(vec3 P, vec3 N, int count, float maxRange)
{
    vec3 acc = vec3(0.0);
    for (int i = 0; i < count; ++i) {
        const vec4 lpos = rtLight[i * 3 + 0];
        const vec4 lcol = rtLight[i * 3 + 1];
        const vec4 ldir = rtLight[i * 3 + 2];

        vec3  ld;
        float dist, ndl;
        const float att = dynLightAtten(lpos, lcol, ldir, P, N, ld, dist, ndl);
        const float w   = att * max(ndl, 0.0);
        // Cheap tests first: most lights are out of range of most hits, and every
        // one rejected here is a shadow ray not traced.
        if (w < 1e-3) continue;

        // Stop just short of the light so the ray cannot hit the fixture it comes
        // from, and never outlive the bubble: past its boundary "no hit" means
        // "nothing built", which would let a lamp shine through a wall that exists.
        if (rtOccluded(P + N * 0.02, ld, 0.01, min(dist * 0.99, maxRange))) continue;

        acc += lcol.rgb * w;
    }
    return acc;
}
#endif  // RT_LIGHTS

#endif  // RT_SHADE_GLSL
