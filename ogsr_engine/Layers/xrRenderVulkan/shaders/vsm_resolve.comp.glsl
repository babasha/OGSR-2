#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM temporal resolve (TAA-for-shadows). One thread per screen
// pixel: reconstruct world from the prepass depth, sample the VSM atlas (the same
// world->light->page->atlas mapping the receiver used to do directly), then blend
// against a reprojected history with an EMA. The clipmap origin is sub-texel JITTERED
// each frame (vk_vsm BeginFrame) so the per-frame texel quantization of the shadow
// edge decorrelates; the EMA averages it → the "crawling snake" along moving-sun
// shadow edges resolves to a smooth, stable soft edge. Output is a screen-space mask
// (R = lit factor, G = distance to this frame's camera, for history disocclusion
// rejection). Receivers then just sample R by screen UV (vsm_sample.glsl). See vk_vsm.cpp.
#include "vsm_common.glsl"

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uDepth;    // current scene prepass depth
layout(set = 0, binding = 1) uniform sampler2D uAtlas;    // STATIC VSM atlas (opaque + trees)
layout(set = 0, binding = 7) uniform sampler2D uAtlasDyn; // DYNAMIC VSM atlas (NPC + grass)
layout(set = 0, binding = 2) readonly buffer VsmPageTable    { uint vsmPageTable[]; };     // STATIC virtual->slot
layout(set = 0, binding = 8) readonly buffer VsmPageTableDyn { uint vsmPageTableDyn[]; };  // DYNAMIC virtual->slot
layout(set = 0, binding = 9) readonly buffer VsmDynUsed      { uint vsmDynUsed[]; };       // dyn slot -> 1 if any NPC/grass caster was binned into it
layout(set = 0, binding = 3) uniform VsmClipmap {
    mat4 view;                 // world -> sun light space
    vec4 level[VSM_LEVELS];    // xy = level origin (light XY of texel 0,0), z = extent (m)
    vec4 zparams;              // x = zNear, y = 1/(zFar-zNear), z = depth-compare bias
} vsmC;
layout(set = 0, binding = 4) uniform sampler2D uHistory; // previous frame's mask (RG = shadow, dist)
layout(set = 0, binding = 5, rgba16f) uniform writeonly image2D uOut;
layout(set = 0, binding = 6) uniform Resolve {
    mat4 invViewProj;    // current clip -> world
    mat4 prevViewProj;   // world -> previous-frame clip (history reproject)
    vec4 prevCamPos;     // xyz = previous frame camera (for history distance check); w = dyn-debug (r_vsm_debug_dyn: write dyn occlusion to mask B)
    vec4 curCamPos;      // xyz = this frame camera (stored as G for next frame); w = history weight on dyn-shadowed pixels (r_vsm_ta_blend_dyn)
    vec4 screen;         // xy = pixel dims, zw = 1/dims
    vec4 params;         // x = history weight (alpha), y = reject tolerance, z = historyValid, w = dyn-gate (1 = skip dyn pages with no casters, r_vsm_dyn_gate)
    vec4 params2;        // x = clamp tol (neighbourhood clamp, r_vsm_ta_clamp), y = motion ref px (r_vsm_ta_motion), z = motion-floor weight (r_vsm_ta_motion_floor), w = slope-bias min (r_vsm_bias_min, normalized; 0 = legacy constant bias)
    vec4 params3;        // SOFT SHADOWS: x = filter taps (0 = legacy 3x3 PCF), y = blocker-search taps, z = tan(sun cone half-angle), w = max blocker search distance (m)
    vec4 params4;        // x = per-frame noise phase (decorrelates the stochastic discs so the EMA averages them), y = no-page history carry (r_vsm_ta_carry; 0 = legacy "flash lit"), zw = reserved
} R;

// VSM atlas sample (3x3 PCF) — mirrors the page mapping vsm_page.vert rasterized with.
// Returns lit factor 1 = lit .. 0 = shadowed; out of clipmap → lit.
// noPage (out) = the receiver IS inside the clipmap but no level holds a resident page
// for it — MISSING DATA, not "no occluder". Returned lit like everything else, but
// flagged so main() can carry the history instead of flashing the sun on (see there).
// dynOcc (out) = fraction of taps occluded by the DYNAMIC atlas alone — only computed
// under r_vsm_debug_dyn (prevCamPos.w), UNGATED by dynUsed: it shows the atlas truth,
// so the tonemap's red overlay reveals NPC/grass shadows even if the gate drops them.
// dynHit (out) = fraction of taps the DYNAMIC atlas shadows (always computed): these
// casters MOVE every frame (wind-swaying crowns, NPCs), so main() cuts the EMA history
// weight there — the full weight drags a many-frame smear ("jelly") behind them.
// statLit (out) = visibility against the STATIC atlas ALONE (trees/buildings, no
// grass/NPC/wind-tree dyn casters) — written to mask B. HISTORY: this was the
// mask-based grass receiver path (grass read B to avoid dyn blade-frequency stripes
// projecting onto the canopy); grass now samples the atlases DIRECTLY at its own
// world pos (vsm_sample VSM_GRASS_DIRECT), so B survives only as a diagnostic
// (r_grass_debug 3 comparison view) — nearly free: same taps, one extra compare.
// nrm/tanT = the receiver plane's depth-reconstructed normal and its slope vs the
// sun ray, from main(). r_vsm_bias_min > 0 enables the modern bias scheme:
//   NORMAL OFFSET — shift the sample point off the surface by ~2 texels along the
//   normal: the acne band (PCF footprint + bilinear + sub-texel jitter) is cleared
//   POSITIONALLY, so the depth bias can stay a small constant (write-side raster
//   bias + D16 quantization) instead of a slope-ballooned slack. The legacy flat
//   0.6 m — and even the round-1 slope-scaled bias with its 0.6 m cap — let sun
//   punch through everything thinner than the slack (plank walls, roofs, corners).
//   The remaining slope term is capped LOW (0.15 m): grazing acne rides the offset.
float sampleVSM(vec3 wp, vec3 nrm, float tanT, out float dynOcc, out float dynHit, out float statLit, out bool noPage)
{
    dynOcc = 0.0;
    dynHit = 0.0;
    statLit = 1.0;
    noPage = false;
    if (R.params2.w > 0.0 && dot(nrm, nrm) > 0.5) {
        vec3 lp0 = (vsmC.view * vec4(wp, 1.0)).xyz;
        vec2 uv0; ivec2 pg0;
        int L0 = vsmSelect(lp0.xy, vsmC.level, uv0, pg0);
        if (L0 >= 0) wp += nrm * (2.0 * vsmC.level[L0].z * (1.0 / 4096.0));
    }
    vec3 lp = (vsmC.view * vec4(wp, 1.0)).xyz;
    vec2 luv; ivec2 page;
    int  L = vsmSelect(lp.xy, vsmC.level, luv, page);
    if (L < 0) return 1.0;

    // Walk COARSER until the STATIC page is mapped. The static table is fully resident
    // for every marked page, so a miss at the finest containing level means the page
    // was marked coarser (throttle LOD bias, r_vsm_throttle) or its scroll-in redraw
    // was deferred (r_vsm_dirty_budget) — sample the next coarser level instead of
    // falling out "lit". One 4-byte read per extra step; the un-throttled steady state
    // exits on the first iteration (identical to the old single lookup).
    // ⚠ The walk only actually LANDS in the throttle case: the mark writes ONE level per
    // pixel, so a coarser page over the near field is marked by nobody and is UNMAPPED
    // too. For a deferred page the walk therefore falls through to "lit" — hence noPage
    // and the history carry in main(); this loop is not a fallback we can lean on.
    uint slotS = VSM_UNMAPPED;
    for (; L < VSM_LEVELS; ++L) {
        vec2 t = (lp.xy - vsmC.level[L].xy) / vsmC.level[L].z;
        page   = clamp(ivec2(floor(t * float(VSM_PAGES_AXIS))), ivec2(0), ivec2(VSM_PAGES_AXIS - 1));
        slotS  = vsmPageTable[vsmPageIndex(L, page)];
        if (slotS < uint(VSM_MAX_PHYS_S)) { luv = t; break; }
    }
    if (L >= VSM_LEVELS) { noPage = true; return 1.0; }   // mapped nowhere → no data (main() carries history)

    int  idx   = vsmPageIndex(L, page);
    uint slotD = vsmPageTableDyn[idx];   // DYNAMIC atlas slot (demand-allocated; 2048 grid)
    bool hasS  = slotS < uint(VSM_MAX_PHYS_S);
    bool resD  = slotD < uint(VSM_MAX_PHYS);
    // The dyn alloc claims a slot for EVERY visible page; only pages some NPC/grass caster
    // actually binned into hold real depth (the rest are cleared-empty) → skip those 9 taps.
    // Gated by r_vsm_dyn_gate (params.w) for live A/B while the flag chain is being verified.
    bool hasD  = resD && (R.params.w < 0.5 || vsmDynUsed[slotD] != 0u);
    bool dbgD  = resD && (R.prevCamPos.w > 0.5);   // debug: always fetch the dyn atlas
    if (!hasS && !hasD && !dbgD) return 1.0;       // page resident in neither atlas → lit

    vec2  pageLocal = luv * float(VSM_PAGES_AXIS) - vec2(page);   // [0,1) within the page
    vec2  baseS = vec2(float(slotS % uint(VSM_ATLAS_W_S)), float(slotS / uint(VSM_ATLAS_W_S)));
    vec2  baseD = vec2(float(slotD % uint(VSM_ATLAS_W)),   float(slotD / uint(VSM_ATLAS_W)));
    float zHere = (lp.z - vsmC.zparams.x) * vsmC.zparams.y;
    float bias  = vsmC.zparams.z;   // legacy flat receiver bias (r_vsm_bias_min 0 path)
    if (R.params2.w > 0.0) {
        // Small constant (r_vsm_bias_min ≈ raster write bias + D16 quantization) +
        // a slope term for the PCF footprint, capped at 0.15 m — the normal offset
        // above carries the grazing-angle acne, a big depth slack only re-opens
        // the thin-wall light leaks this scheme exists to close.
        float texelW = vsmC.level[L].z * (1.0 / 4096.0);          // VSM_VIRTUAL_RES
        float slopeN = 2.2 * texelW * tanT * vsmC.zparams.y;      // metres -> normalized z
        bias = R.params2.w + min(slopeN, 0.15 * vsmC.zparams.y);
    }
    float biasD = vsmC.zparams.w;   // DYNAMIC-atlas receiver bias (tiny: no ground in that atlas)
    const vec2 dimS = vec2(float(VSM_ATLAS_W_S), float(VSM_ATLAS_H_S));   // static atlas (6144)
    const vec2 dimD = vec2(float(VSM_ATLAS_W),   float(VSM_ATLAS_H));     // dynamic atlas (2048)

    const float tp    = 1.0 / float(VSM_PAGE_SIZE);   // one page texel, page-local units
    const float inset = 0.5 * tp;
    float lit = 0.0, litS = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        vec2 pl = clamp(pageLocal + vec2(float(dx), float(dy)) * tp, vec2(inset), vec2(1.0 - inset));
        // Each atlas gets its OWN receiver bias. The static atlas holds the ground
        // itself, so its bias (zparams.z) must absorb terrain self-shadow acne — but at
        // a 2000 m clipmap z-range 0.0003 normalized = 0.6 m along the sun ray, which
        // also REJECTED every dyn occluder closer than 0.6 m: grass blade parts below
        // ~half a metre cast nothing (high sun shrinks the gap = H/sin(elev) further) —
        // tuft shadows went missing/cut while tall trees were unaffected. The DYNAMIC
        // atlas holds ONLY casters (grass/NPC/near-crowns), NEVER the receiving ground,
        // so it cannot acne against it → a tiny epsilon (zparams.w, r_vsm_bias_dyn:
        // D16 quantization + the write-side raster bias) keeps low blades shadowing.
        float occS = 1.0;
        if (hasS) occS = min(occS, texture(uAtlas, (baseS + pl) / dimS).r);
        bool sh = (zHere - bias > occS);
        if (hasD || dbgD) {
            float d = texture(uAtlasDyn, (baseD + pl) / dimD).r;
            if (hasD && (zHere - biasD > d)) {
                sh = true;
                dynHit += 1.0 / 9.0;
            }
            // Debug: mode 1 counts only the VISIBLE darkening the dyn atlas causes — taps
            // the static atlas leaves lit but the dyn depth shadows. (Raw dyn occlusion
            // painted whole shadow COLUMNS through houses/crowns — technically correct
            // atlas content, but already dark in the real image = pure confusion.)
            // Mode 2 (r_vsm_debug_dyn 2) = RAW: any dyn occlusion, ungated — for auditing
            // whether a caster made it into the atlas at all (grass pair coverage).
            // Mode 3 (r_vsm_debug_dyn 3) = PRESENCE: any caster DEPTH at this receiver's
            // texel, ignoring the depth comparison — splits "fragments never rasterized
            // into the page" (no red in 3) from "depth written but the test rejects it"
            // (red in 3, none in 2).
            if (dbgD) {
                if (R.prevCamPos.w > 2.5) { if (d < 0.999) dynOcc += 1.0 / 9.0; }
                else if ((R.prevCamPos.w > 1.5 || zHere - bias <= occS) && (zHere - biasD > d)) dynOcc += 1.0 / 9.0;
            }
        }
        lit  += sh ? 0.0 : 1.0;
        litS += (zHere - bias > occS) ? 0.0 : 1.0;   // static atlas alone (grass receivers)
    }
    statLit = litS * (1.0 / 9.0);
    return lit * (1.0 / 9.0);
}

// ===========================================================================
// SOFT SHADOWS — stochastic PCSS over the VSM atlases (r_vsm_soft > 0).
//
// The 3x3 PCF above blurs by a FIXED texel radius: a post's shadow at its own
// base is exactly as soft as a crown's from 15 m up. This path gives the real
// thing — contact-hard, distance-soft — in two stochastic passes:
//   (1) BLOCKER SEARCH: taps in a disc of radius tan(theta)*range around the
//       receiver; every tap nearer the light than the receiver is a blocker.
//       Their average depth gives the blocker distance D.
//   (2) FILTER: taps in a disc of radius w = tan(theta)*D — the geometric
//       penumbra cast by a source of angular radius theta. D small (contact)
//       collapses w to one texel = hard edge; D large (canopy) = wide and soft.
// Both discs are VOGEL spirals rotated by a per-pixel + per-frame hash, so the
// sampling error is NOISE, not banding — and noise is precisely what the
// temporal resolve below already exists to eat (clipmap jitter + EMA +
// neighbourhood clamp). Owning that denoiser is why stochastic beats a regular
// grid here; a fixed kernel would have to pay every tap every frame.
//
// Deliberately NOT a depth-march (UE5's SMRT): "first sample behind the depth
// surface" can never un-occlude a ray that started inside the shadow volume, so
// the umbra GROWS outward instead of the edge softening about the geometric
// boundary. That is invisible at the sun's true 0.53 deg, but this engine
// exposes an exaggerated cinematic angle, where one-sided penumbra reads as
// fat, bloated shadows. PCSS is symmetric at any angle.
// ===========================================================================
const vec2  kDimS  = vec2(float(VSM_ATLAS_W_S), float(VSM_ATLAS_H_S));   // static atlas, pages
const vec2  kDimD  = vec2(float(VSM_ATLAS_W),   float(VSM_ATLAS_H));     // dynamic atlas, pages
const float kInset = 0.5 / float(VSM_PAGE_SIZE);                         // half a texel, page-local
const float kMaxNormalOffset = 0.25;                                     // m — cap on the filter-scaled normal lift (peter-panning)

// Vogel disc: i-th of n points on a golden-angle spiral, rotated by phi. Even
// coverage at ANY n (no power-of-two requirement) for one sin/cos per tap.
vec2 vsmVogel(int i, int n, float phi)
{
    float r = sqrt((float(i) + 0.5) / float(n));
    float a = float(i) * 2.39996323 + phi;
    return r * vec2(cos(a), sin(a));
}

// Interleaved gradient noise (Jimenez) — the cheap blue-ish per-pixel hash that
// temporal filters resolve well. The frame phase rotates it over time so the
// EMA averages DIFFERENT taps each frame instead of baking one pattern in.
float vsmIGN(vec2 px, float phase)
{
    return fract(52.9829189 * fract(dot(px + 5.588238 * phase, vec2(0.06711056, 0.00583715))));
}

// Page-table lookup at a FIXED clipmap level for arbitrary light-space XY (the
// filter disc walks across page boundaries). The level stays the receiver's:
// changing it mid-filter would mix texel scales inside one estimate. A tap on
// an unmapped page is DROPPED by the caller rather than counted lit — dropping
// keeps the ratio unbiased, counting would punch holes in the shadow.
bool vsmPageAt(vec2 lxy, int L, out vec2 pl, out uint slotS, out uint slotD)
{
    pl = vec2(0.0); slotS = VSM_UNMAPPED; slotD = VSM_UNMAPPED;
    vec2 t = (lxy - vsmC.level[L].xy) / vsmC.level[L].z;
    if (any(lessThan(t, vec2(0.0))) || any(greaterThanEqual(t, vec2(1.0)))) return false;
    ivec2 pg  = ivec2(floor(t * float(VSM_PAGES_AXIS)));
    int   idx = vsmPageIndex(L, pg);
    slotS = vsmPageTable[idx];
    slotD = vsmPageTableDyn[idx];
    pl    = clamp(t * float(VSM_PAGES_AXIS) - vec2(pg), vec2(kInset), vec2(1.0 - kInset));
    return slotS < uint(VSM_MAX_PHYS_S);
}

float vsmDepthS(uint slot, vec2 pl)
{
    vec2 base = vec2(float(slot % uint(VSM_ATLAS_W_S)), float(slot / uint(VSM_ATLAS_W_S)));
    return texture(uAtlas, (base + pl) / kDimS).r;
}
float vsmDepthD(uint slot, vec2 pl)
{
    vec2 base = vec2(float(slot % uint(VSM_ATLAS_W)), float(slot / uint(VSM_ATLAS_W)));
    return texture(uAtlasDyn, (base + pl) / kDimD).r;
}

// Stochastic PCSS. Same contract as sampleVSM (1 = lit .. 0 = shadowed) so main()
// treats the two paths identically. `rnd` = (disc rotation, unused) from the
// per-pixel/per-frame hash.
float sampleVSMSoft(vec3 wp, vec3 nrm, float tanT, float rot,
                    out float dynOcc, out float dynHit, out float statLit, out bool noPage)
{
    dynOcc = 0.0; dynHit = 0.0; statLit = 1.0; noPage = false;
    bool haveN = dot(nrm, nrm) > 0.5;

    vec3 lp = (vsmC.view * vec4(wp, 1.0)).xyz;
    vec2 uv0; ivec2 pg0;
    int  L = vsmSelect(lp.xy, vsmC.level, uv0, pg0);
    if (L < 0) return 1.0;
    // Walk COARSER until the STATIC page is mapped — same policy as the PCF path
    // (throttle LOD bias / deferred scroll-in leave finer pages unmapped).
    for (; L < VSM_LEVELS; ++L) {
        vec2 t = (lp.xy - vsmC.level[L].xy) / vsmC.level[L].z;
        if (any(lessThan(t, vec2(0.0))) || any(greaterThanEqual(t, vec2(1.0)))) continue;
        ivec2 pg = ivec2(floor(t * float(VSM_PAGES_AXIS)));
        if (vsmPageTable[vsmPageIndex(L, pg)] < uint(VSM_MAX_PHYS_S)) break;
    }
    if (L >= VSM_LEVELS) { noPage = true; return 1.0; }   // no resident page → main() carries history

    float texelW = vsmC.level[L].z / float(VSM_VIRTUAL_RES);   // metres per virtual texel at L
    float zScale = vsmC.zparams.y;                             // metres -> normalized depth
    float tanS   = R.params3.z;                                // tan(sun cone half-angle)
    float rMax   = max(tanS * R.params3.w, 2.0 * texelW);      // blocker-search radius, light-space m

    // Does this page hold any DYNAMIC caster at all? Decided ONCE at the receiver
    // (the same call the dyn-gate makes for the PCF path): the filter disc is
    // small, and re-testing per tap would double the storage traffic of both
    // passes for a flag that is uniform across a page in all but edge cases.
    vec2 plC; uint slotSC, slotDC;
    vsmPageAt(lp.xy, L, plC, slotSC, slotDC);
    bool resD = slotDC < uint(VSM_MAX_PHYS);
    bool useD = resD && (R.params.w < 0.5 || vsmDynUsed[slotDC] != 0u);
    bool dbgD = resD && (R.prevCamPos.w > 0.5);

    // ---- PASS 1: blocker search.
    // Lift off the surface by 2 texels along the normal (the PCF path's scheme):
    // clears the acne band POSITIONALLY so the depth bias can stay small. The
    // per-tap slope term below then only has to cover the tap's own lateral
    // offset on the receiver plane.
    vec3 wpS = haveN ? wp + nrm * (2.0 * texelW) : wp;
    vec3 lpS = (vsmC.view * vec4(wpS, 1.0)).xyz;
    float zS = (lpS.z - vsmC.zparams.x) * zScale;
    float biasC = (R.params2.w > 0.0) ? R.params2.w : vsmC.zparams.z;   // constant part
    float biasD = vsmC.zparams.w;                                       // dyn atlas: casters only, tiny epsilon

    int   ns   = max(int(R.params3.y), 1);
    float sumD = 0.0, cntD = 0.0;
    for (int i = 0; i < ns; ++i) {
        // Tap 0 sits AT the receiver so a contact blocker can never be missed by
        // the stochastic offsets — that tap alone reproduces the classic test.
        vec2 off = (i == 0) ? vec2(0.0) : vsmVogel(i, ns, rot * 6.2831853) * rMax;
        vec2 pl; uint sS, sD;
        if (!vsmPageAt(lpS.xy + off, L, pl, sS, sD)) continue;
        float r     = length(off);
        float slope = min(r * tanT * zScale, 0.15 * zScale);   // low cap: the normal lift carries grazing acne
        // Each atlas keeps its OWN bias (the static one holds the receiving
        // ground and must absorb its self-shadow acne; the dynamic one holds
        // casters only) — so they are tested separately, then the NEARER
        // confirmed blocker of the two sizes the penumbra.
        float dBest = 2.0;
        float dS = vsmDepthS(sS, pl);
        if (dS < zS - (biasC + slope)) dBest = dS;
        if (useD && sD < uint(VSM_MAX_PHYS)) {
            float dD = vsmDepthD(sD, pl);
            if (dD < zS - biasD) dBest = min(dBest, dD);
        }
        if (dBest < 1.5) { sumD += dBest; cntD += 1.0; }
    }

    // Penumbra radius from the AVERAGE blocker distance. No blocker found at all
    // => fully lit, and both output channels follow (no filter pass to run).
    if (cntD < 0.5) { statLit = 1.0; return 1.0; }
    float distB = max(zS - sumD / cntD, 0.0) / max(zScale, 1e-9);   // normalized -> metres
    float w     = clamp(tanS * distB, 0.75 * texelW, rMax);

    // ---- PASS 2: filter over a disc of radius w.
    // The normal lift now scales with the FILTER footprint (a wide kernel reaches
    // further down the receiver plane, so it needs a proportionally bigger lift),
    // capped so the shadow cannot visibly detach from its caster.
    vec3 wpF = haveN ? wp + nrm * min(max(w, 2.0 * texelW), kMaxNormalOffset) : wp;
    vec3 lpF = (vsmC.view * vec4(wpF, 1.0)).xyz;
    float zF = (lpF.z - vsmC.zparams.x) * zScale;

    int   nf = max(int(R.params3.x), 1);
    float lit = 0.0, litS = 0.0, taken = 0.0, hitD = 0.0, occDbg = 0.0;
    // Offset the filter rotation from the search rotation: reusing one angle
    // would line the two discs up and correlate the estimate with its own input.
    float rotF = rot * 6.2831853 + 1.61803399;
    for (int i = 0; i < nf; ++i) {
        vec2 off = vsmVogel(i, nf, rotF) * w;
        vec2 pl; uint sS, sD;
        if (!vsmPageAt(lpF.xy + off, L, pl, sS, sD)) continue;
        taken += 1.0;
        float r     = length(off);
        float slope = min(r * tanT * zScale, 0.15 * zScale);
        float bias  = biasC + slope;
        float occS  = vsmDepthS(sS, pl);
        bool  shS   = (zF - bias > occS);
        bool  sh    = shS;
        if ((useD || dbgD) && sD < uint(VSM_MAX_PHYS)) {
            float d = vsmDepthD(sD, pl);
            if (useD && (zF - biasD > d)) { sh = true; hitD += 1.0; }
            // Debug overlay modes — same meanings as the PCF path (see sampleVSM):
            // 1 = only the darkening the dyn atlas actually adds, 2 = raw dyn
            // occlusion, 3 = any caster DEPTH present at this texel.
            if (dbgD) {
                if (R.prevCamPos.w > 2.5) { if (d < 0.999) occDbg += 1.0; }
                else if ((R.prevCamPos.w > 1.5 || !shS) && (zF - biasD > d)) occDbg += 1.0;
            }
        }
        lit  += sh  ? 0.0 : 1.0;
        litS += shS ? 0.0 : 1.0;
    }
    if (taken < 0.5) { noPage = true; return 1.0; }   // whole disc landed on unmapped pages → no data
    float inv = 1.0 / taken;
    dynHit  = hitD   * inv;
    dynOcc  = occDbg * inv;
    statLit = litS   * inv;
    return lit * inv;
}

// Reconstruct world from the prepass depth (D3D NDC, y-up — matches vsm_mark.comp / ssao.frag).
vec3 reconWorld(vec2 uv)
{
    float zndc = texture(uDepth, uv).r;
    vec4  clip = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, zndc, 1.0);
    vec4  w    = R.invViewProj * clip;
    return w.xyz / w.w;
}

void main()
{
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= int(R.screen.x) || px.y >= int(R.screen.y)) return;

    vec2  uv   = (vec2(px) + 0.5) * R.screen.zw;
    float zndc = texture(uDepth, uv).r;
    if (zndc >= 0.99999) { imageStore(uOut, px, vec4(1.0, 1e6, 1.0, 0.0)); return; }   // sky → lit (B too)

    vec4 clip  = vec4(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y, zndc, 1.0);
    vec4 world = R.invViewProj * clip;
    vec3 wp    = world.xyz / world.w;

    // Receiver slope vs the sun ray for the slope-scaled bias. Normal from depth via
    // central-min differences: at a silhouette the one-sided difference jumps across
    // the depth gap and fabricates a near-grazing plane → pick the smaller-step side
    // (both sides discontinuous is a 1px sliver; the tan clamp bounds the damage).
    float tanT = 0.0;
    vec3  nrm  = vec3(0.0);
    bool  softOn = R.params3.x >= 1.0;            // r_vsm_soft: stochastic PCSS instead of 3x3 PCF
    if (R.params2.w > 0.0 || softOn) {            // the soft path needs the plane too (normal lift + per-tap slope bias)
        vec3 wxp = reconWorld(uv + vec2(R.screen.z, 0.0)), wxn = reconWorld(uv - vec2(R.screen.z, 0.0));
        vec3 wyp = reconWorld(uv + vec2(0.0, R.screen.w)), wyn = reconWorld(uv - vec2(0.0, R.screen.w));
        vec3 dx = (dot(wxp - wp, wxp - wp) < dot(wp - wxn, wp - wxn)) ? wxp - wp : wp - wxn;
        vec3 dy = (dot(wyp - wp, wyp - wp) < dot(wp - wyn, wp - wyn)) ? wyp - wp : wp - wyn;
        vec3 n  = cross(dx, dy);
        float nl = length(n);
        if (nl > 1e-8) {
            nrm = n / nl;
            // Orient off the surface toward the CAMERA (the reconstructed winding is
            // arbitrary): the offset must lift the sample into open air, not sink it.
            if (dot(nrm, R.curCamPos.xyz - wp) < 0.0) nrm = -nrm;
            vec3 sunTravel = normalize(vec3(vsmC.view[0].z, vsmC.view[1].z, vsmC.view[2].z));
            float c = abs(dot(nrm, sunTravel));
            tanT = min(sqrt(max(1.0 - c * c, 0.0)) / max(c, 0.05), 12.0);
        } else {
            tanT = 12.0;   // degenerate plane → max (capped) slope slack, no offset
        }
    }

    float dynOcc, dynHit, statLit;
    bool  noPage;
    // Per-pixel + per-frame disc rotation. Both stochastic discs derive from this
    // one hash, so a pixel's search and filter stay coherent within a frame and
    // decorrelate across frames — which is what lets the EMA below average them.
    float rot  = vsmIGN(vec2(px), R.params4.x);
    float cur  = softOn ? sampleVSMSoft(wp, nrm, tanT, rot, dynOcc, dynHit, statLit, noPage)
                        : sampleVSM(wp, nrm, tanT, dynOcc, dynHit, statLit, noPage);

    // Debug refinement: a surface FACING AWAY from the sun gets no direct light — a dyn
    // shadow there changes nothing on screen (e.g. a ceiling under a roof hole crossed by
    // a distant NPC's shadow column). Reconstruct the geometric normal from depth and
    // drop the red on backfacing/grazing pixels.
    if (dynOcc > 0.0 && R.prevCamPos.w < 1.5) {   // mode 2 = raw: keep backfacing/grazing red too
        vec3 wpX = reconWorld(uv + vec2(R.screen.z, 0.0));
        vec3 wpY = reconWorld(uv + vec2(0.0, R.screen.w));
        vec3 n   = cross(wpX - wp, wpY - wp);
        if (dot(n, R.curCamPos.xyz - wp) < 0.0) n = -n;   // orient towards the camera
        // Light travel direction (world): row 2 of the world->light view (light-space +Z).
        vec3 sunTravel = normalize(vec3(vsmC.view[0].z, vsmC.view[1].z, vsmC.view[2].z));
        if (dot(normalize(n), -sunTravel) < 0.05) dynOcc = 0.0;
    }
    float dist = length(wp - R.curCamPos.xyz);   // stored for next frame's reject test

    // ---- History fetch (reproject + disocclusion reject). Hoisted out of the blend
    // below because the no-page carry needs the SAME sample and the SAME validity test.
    vec4  hist     = vec4(0.0);
    bool  histOK   = false;
    float motionPx = 0.0;
    if (R.params.z > 0.5) {                       // history valid (not first frame / no resize)
        vec4 pc = R.prevViewProj * vec4(wp, 1.0);
        if (pc.w > 0.0) {
            vec2 puv = (pc.xy / pc.w) * vec2(0.5, -0.5) + 0.5;   // prev-frame screen UV (same y-flip)
            if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)))) {
                hist = texture(uHistory, puv);                   // (shadowPrev, distFromPrevCam, dbg, dynHitPrev)
                float expectPrev = length(wp - R.prevCamPos.xyz);
                // Same static surface last frame → stored distance ≈ expected. Reject
                // (use current only) on disocclusion so silhouettes don't ghost.
                if (abs(hist.r) <= 1.0001 && abs(hist.g - expectPrev) <= R.params.y * expectPrev + 0.05) {
                    histOK   = true;
                    motionPx = length((puv - uv) * R.screen.xy);
                }
            }
        }
    }

    float outShadow = cur;
    if (noPage) {
        // ---- NO RESIDENT PAGE = MISSING DATA, NOT "NO OCCLUDER".
        // The old policy returned LIT here, which is the single worst value it could
        // pick: the sun switches on across whatever the unmapped pages cover. And they
        // go unmapped in BURSTS — vsm_resid defers every scrolled-in (wrong-tile) page
        // over r_vsm_dirty_budget (128) to a later frame, and a sun step / window snap
        // makes hundreds of pages wrong in ONE frame (measured: wrong max 487..736 per
        // frame against ~480 pages the eye sees, deferred 950..4300 per 3 s window).
        // ~400 pages then read "lit" for the few frames the budget needs to drain them.
        // The EMA hid that while standing still (weight 0.9), but the motion-adaptive
        // fade (r_vsm_ta_motion_floor 0.30) drops the history exactly when the camera
        // moves — which is why the blink only showed up while RUNNING.
        // Carry the reprojected history instead: the shadow goes a few frames stale
        // (invisible) rather than vanishing (very visible). r_vsm_ta_carry < 1 decays a
        // page that never comes back toward lit over ~a second instead of freezing its
        // shadow forever; 0 = the old flash-lit behaviour for A/B.
        outShadow = histOK ? mix(cur, hist.r, R.params4.y) : cur;
    } else if (histOK) {
        // Dyn-atlas casters move every frame → their shadow edge is somewhere
        // ELSE each frame; the full EMA (tuned to average the static edge's
        // texel quantization) smears them into a trail. Where the dyn atlas
        // shadows this pixel now — or did last frame (hist.a covers the
        // trailing edge) — drop to the dyn history weight (r_vsm_ta_blend_dyn).
        float a = (dynHit > 0.0 || hist.a > 0.0) ? min(R.params.x, R.curCamPos.w) : R.params.x;

        // (2) MOTION-ADAPTIVE weight. The distance reject above only catches
        // DISocclusion (surface changed) — but high-frequency foliage shadows on
        // the SAME ground (same depth) pass it, and under camera motion the
        // bilinear history fetch at `puv` blurs a bit more each frame → the full
        // EMA accumulates that blur into "каша" (only while moving). Fade the
        // weight toward a floor by the reprojected screen motion (px): still
        // camera keeps the full EMA (clean coarse shadow + sub-texel detail),
        // moving camera stops compounding the blur.
        float mfade = clamp(motionPx / max(R.params2.y, 1e-3), 0.0, 1.0);
        a = mix(a, min(a, R.params2.z), mfade);

        // (1) NEIGHBOURHOOD CLAMP (value space). Bound the history sample to the
        // current shadow ±tol before the blend so a stale value can't drag a
        // many-frame trail (the classic TAA anti-ghost, done per-pixel without
        // the extra sampleVSM taps a spatial min/max would cost): sub-texel
        // jitter within tol still averages (keeps the AA), a big deviation
        // (moved foliage edge) is clamped out (kills the smear).
        float hClamped = clamp(hist.r, cur - R.params2.x, cur + R.params2.x);
        outShadow = mix(cur, hClamped, a);
    }
    // B = STATIC-only visibility — diagnostic channel (r_grass_debug 3; grass shading
    // now samples the atlases directly, see sampleVSM's statLit note). Raw, no EMA.
    // Under r_vsm_debug_dyn the debug dynOcc takes the channel over (the red overlay).
    // Mode 4 = SUN-VISIBILITY forensics: B carries the final resolved lit factor —
    // the tonemap's red overlay then marks every pixel the shadow system deems
    // SUN-LIT (leak triage: bright wall patch + red = shadow leak; + no red = the
    // light is NOT the sun term — ambient/hemi hunt instead).
    // A = this frame's dynHit (next frame's trailing-edge EMA cut).
    float bOut = (R.prevCamPos.w > 3.5) ? outShadow
               : (R.prevCamPos.w > 0.5) ? dynOcc : statLit;
    imageStore(uOut, px, vec4(outShadow, dist, bOut, dynHit));
}
