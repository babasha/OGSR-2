#ifndef DDGI_COMMON_GLSL
#define DDGI_COMMON_GLSL
// xrRenderVulkan — DDGI probe volume: addressing, octahedral maps, and the
// irradiance lookup. Shared by the trace pass, both blend passes and every
// consumer, because all four have to agree on the same three conventions and a
// second copy of any of them would drift silently.
//
// THE VOLUME IS TOROIDAL, NOT SLIDING
// -----------------------------------
// A probe grid that follows the camera has one hard problem: when the volume
// moves, every probe's history must still belong to the place it was measured.
// If slot (i,j,k) simply meant "origin + i*spacing", then moving the origin by
// one cell would silently re-point every slot at its neighbour's world position
// and smear a frame of history across the whole grid every step the player takes.
//
// So slots are addressed by ABSOLUTE world grid coordinate modulo the grid size:
// a probe keeps its slot for as long as it stays inside the window, and only the
// planes that scroll IN are stale. Those are detected by comparing against the
// previous frame's window (`prevBase`) and blended with zero hysteresis, so a new
// probe takes its first measurement whole instead of inheriting a stranger's.
//
// A probe's world position is `absCoord * spacing` — anchored to the world, never
// to the camera, so it does not jitter as the player moves.
//
// CASCADES, AND WHY THEY GO OUTWARD AND NOT INWARD
// ------------------------------------------------
// One window of a fixed spacing has to choose between reach and detail. idTech 8
// answers with 6 nested cascades and updates ONE of them per frame; the same trade
// is taken here. Cascade c has the same probe count as cascade 0 but twice its
// spacing, so it covers twice the distance at half the density.
//
// ⚠ The cascades EXTEND the volume outward — cascade 0 keeps whatever spacing
// r_ddgi_spacing names, and the ladder only ever doubles from there. A 2 m spacing
// was measured to print lattice-shaped blotches on open ground, because probe
// planes land just UNDER the terrain, trace nothing but immediate hits and go dark;
// that defect is the reason 3 m is the outdoor default. PROBE RELOCATION
// (r_ddgi_reloc, ddgi_relocate.comp.glsl) is the direct answer to it — a probe
// that lands in the ground steps back out of it — so the spacing can eventually be
// chosen for reach rather than for the worst-placed probe.
//
// ⚠⚠ INDOORS THE SAME LADDER FAILS FROM THE OTHER END, and that is why the cascade
// ceiling is 6 rather than 4. A room 4 m across holds NO valid probe at 3 m
// spacing: all eight neighbours of any interior point stand in a wall or outside
// the building, the Chebyshev depth test rejects every one of them, and the lookup
// returns zero — which also kills the bounce, since the bounce reads this same
// field. Measured on Dark Valley 22-08: r_ddgi_spacing 1.0 brings the room back.
// The cost is reach, because reach = dim_xz * spacing0 * 2^(casc-1): at 1 m and 3
// cascades the whole ladder spans 64 m instead of 192. Cascades 5-6 buy that back
// (1,2,4,8,16 m => 256 m) for one more cascade's atlas and a slower round-robin.
// A spacing that adapts per REGION rather than per shell is the real answer and
// does not exist yet; this is the honest interim, and it is a user's choice, not
// a default.
//
// ONE CASCADE PER FRAME
// ---------------------
// Tracing and blending every probe every frame is what made the volume cost 0.65 ms
// where idTech 8 spends 0.08. Only the cascade named by `layout_.w` is updated, so
// per-frame work is one cascade's worth regardless of how many exist.
//
// ⚠ CONSEQUENCE: a cascade's `base[c]` must NOT move on frames when that cascade is
// not updated — it describes where that cascade's ATLAS CONTENT is, not where the
// camera is. Moving it early would re-point every slot at a neighbour, which is the
// exact failure the toroidal addressing exists to prevent, just spread over frames.
//
// OCTAHEDRAL MAPS WITH A ONE-TEXEL BORDER
// ---------------------------------------
// Each probe owns a small square of atlas: DDGI_IRR_N^2 texels of irradiance and
// DDGI_DEP_N^2 of distance moments, each surrounded by a one-texel border. The
// border is not padding — bilinear filtering at the edge of an octahedral map has
// to wrap to the OPPOSITE edge (mirrored), and the border texels are what make a
// plain sampler2D do that. Without them every probe shows a seam.
//
// Requires: the includer defines DDGI_SET and DDGI_B_CONSTS before including.
// Define DDGI_SAMPLE (plus DDGI_B_IRR / DDGI_B_DEP) to also get the atlas
// samplers and ddgiSampleIrradiance().

const int DDGI_IRR_N = 6;    // irradiance texels per probe side (8 with border)
const int DDGI_DEP_N = 14;   // distance texels per probe side (16 with border)

const int DDGI_MAX_CASCADES = 6;

layout(std140, set = DDGI_SET, binding = DDGI_B_CONSTS) uniform DDGIConstsBlock {
    // PER CASCADE. xyz = low corner of that cascade's toroidal window, in ABSOLUTE
    // grid coords OF THAT CASCADE (its own lattice, spacing = grid.x << c).
    ivec4 base[DDGI_MAX_CASCADES];
    // The same, as of that cascade's PREVIOUS UPDATE — which with interleaving is
    // several frames ago, not last frame. Drives the zero-hysteresis test.
    ivec4 prevBase[DDGI_MAX_CASCADES];
    ivec4 dims;       // xyz = grid dimensions in probes (identical for every cascade); w = rays per probe
    ivec4 layout_;    // x = probes per atlas row; y = SH9 sky live; z = sky cube live; w = cascade updated THIS frame
    // x = cascade count, y = probes per cascade, z = dynamic lights in the RT world.
    // w = GI ALBEDO BOOST x256 (r_rt_albedo). Integer because this slot was the
    // reserved one and widening the block would move every field after it.
    ivec4 casc;
    vec4  grid;       // x = CASCADE 0 spacing (m), y = hysteresis, z = normal bias (m), w = depth sharpness
    vec4  rayRot;     // per-frame rotation quaternion for the ray set
    vec4  sunDir;     // xyz = direction TO the sun (unit), w = max ray distance (m)
    vec4  sunColor;   // rgb, linear; w = sky irradiance scale (the receiver's L.sky_params.y)
    vec4  skyColor;   // rgb linear (flat fallback when layout_.y == 0), w = energy scale
    vec4  params;     // x = max probe distance (depth ceiling), y = view bias (m), z = irradiance clamp, w = bounce feedback
    // PROBE RELOCATION. x = first atlas ROW of the offset strip, y = its width in
    // texels, z = relocation live (0 = every probe sits exactly on its lattice
    // point, and every read below compiles to the old arithmetic), w = reserved.
    // Appended at the TAIL so every offset above it is unchanged — the C++ mirror
    // in vk_ddgi.cpp has a static_assert that catches the day this stops being true.
    ivec4 reloc;
    // INDOOR REFLECTANCE COMPENSATION (r_gi_indoor / r_gi_indoor_dist).
    // x = boost (1 = off), y = how far straight up the ceiling test looks (m).
    // ⚠ Appended at the TAIL, so every offset above is unchanged and a shader that
    // does NOT use this still reads the same words with its existing .spv - only
    // the three passes that call rtIndoorUp need recompiling.
    vec4  indoor;
    // EDGE-OF-WINDOW ENERGY CEILING (r_ddgi_edge_clamp). x = k: in the cascade
    // cross-fade the COARSE cascade's luminance is capped at k x the FINE one's
    // before the mix. The measured leak (28-08): an interior wall 25 m out sits in
    // cascade 2's two-cell fade band, so 96% of its answer comes from cascade 3 —
    // whose 8 m lattice holds no probe inside any room and answers "street" (0.9)
    // against the fine cascade's honest 0.06-0.11. A ceiling rather than a blend
    // weight because outdoors the two cascades AGREE (cap never fires, the fade
    // keeps doing its anti-ring job) and indoors the disagreement is the leak
    // itself. 0 = off, the old arithmetic bit-exact.
    //   y = r_gather_clamp     — the gather's OWN per-ray firefly ceiling
    //   z = r_gather_scache_blur — prefilter the screen cache's hit (5 taps)
    //   w = r_gather_rays_edge   — extra rays where depth curvature says the
    //                              hemisphere is cut by thin geometry
    // Appended at the TAIL — same contract as `indoor` above.
    vec4  edge;
    // x = r_gather_lazy: skip the probe-lattice walk on hits where the screen or
    // world cache already answers with full confidence, i.e. where the lattice's
    // blend weight (1-scw)*(1-ww) is zero anyway. It is a PURE COST knob — the
    // arithmetic it skips contributes nothing — and it exists as a knob only so
    // the saving can be measured in one run against the same geometry.
    //   y = r_gather_wrc        — the world-cache query inside the gather
    //   z = r_gather_indoor_ray — the SECOND trace per pixel (the ceiling test)
    // Both are there so r_gather_profile can price each link of the ray on its
    // own; y and z are measurement switches, not quality knobs. w reserved.
    // Tail append again.
    vec4  edge2;
    // RTXGI-CONFORMANCE SWITCHES (review 23-09). Tail append, same contract as
    // `indoor`: every offset above is unchanged. All four are 0 = the old path.
    //   x = r_ddgi_hit_sky    — a probe ray's HIT takes its sky light from the field
    //                           (i.e. through the probes' own misses, occluded)
    //                           instead of the unoccluded SH9 sky + the field on top
    //   y = r_ddgi_crush_tri  — crush the wrap x Chebyshev weight, THEN multiply the
    //                           trilinear weight (RTXGI order); 0 = crush the product
    //   z = fixed rays F      — r_ddgi_fixed_rays: rays [0,F) of every probe are an
    //                           UNROTATED set used only by relocation/classification;
    //                           [F, dims.w) are the rotated set the blends integrate
    //   w = r_ddgi_skip_buried — the lookup skips probes relocation flagged buried
    ivec4 fix;
    // PROBE CLASSIFICATION (r_ddgi_classify). x = on, y = refresh period in updates
    // of the cascade (an inactive probe is still re-traced once per period, so it
    // follows time of day and the froxel fog that reads open-air probes), z = this
    // cascade's update counter, w reserved.
    ivec4 classify;
} ddgi;

// Offset-strip flags (w of ddgiOffsetTexel), as written by ddgi_relocate.comp.
// A FLAG FIELD: readers mask the bit they mean.
const int DDGI_FLAG_MOVED    = 1;   // jumped on its last update — blends reset history
const int DDGI_FLAG_BURIED   = 2;   // still inside solid geometry where it was traced
const int DDGI_FLAG_INACTIVE = 4;   // classification: no geometry within its cell box

// Cascade c is twice as coarse and twice as wide as c-1.
float ddgiSpacing(int c) { return ddgi.grid.x * float(1 << c); }

// The distance ceiling THIS cascade's moments are stored against — what a missed
// ray records ("nothing in the way") and the cap every hit distance is clamped to
// before it enters the mean.
//
// ⚠⚠ IT MUST EXCEED THE CELL DIAGONAL OR THE CASCADE REJECTS ITSELF. The Chebyshev
// test in ddgiSampleCascade compares the distance from the shaded point to the
// probe — up to spacing*sqrt(3) at the far corner of a cell — against the mean this
// map holds. A probe standing in open ground has no hits in most directions, so its
// mean IS the ceiling, and any point further away than the ceiling is declared
// "probably occluded" and has its weight crushed to 0.05 in the middle of an empty
// field. r_ddgi_probe_dist is 24 m: 24x cascade 0's cell and utterly safe there, but
// cascade 4's cell is 16 m and its diagonal is 27.7 m — past the ceiling. ⭐ That is
// why the ladder could not be made SPARSER: every coarser cascade spends more of
// itself self-rejecting. The lookup biases are already doubled per cascade for the
// same reason (see ddgiSampleCascade); this quantity was the one that was not.
//
// ⭐ max(), not a plain scale: a cascade whose cells are small keeps r_ddgi_probe_dist
// EXACTLY, so nothing changes for cascades 0-2 at the shipping spacing and an A/B
// measures only the cascades that were broken.
// ⚠ The 240 m cap dates from the RG16F atlas (it stores d*d, and 256^2 overflows
// fp16). The atlas is RG32F now, but the RG16F fallback (a device that cannot
// filter RG32F) still needs it, so it stays.
float ddgiProbeDist(int c)
{
    return min(max(ddgi.params.x, ddgiSpacing(c) * 4.0), 240.0);
}

// ---------------------------------------------------------------------------
//  Octahedral mapping (Cigolle et al.). Unit sphere <-> [-1,1]^2.
// ---------------------------------------------------------------------------
vec2 ddgiSignNotZero(vec2 v)
{
    return vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

vec2 ddgiOctEncode(vec3 n)
{
    const float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    const vec2  p  = n.xy * (1.0 / max(l1, 1e-8));
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * ddgiSignNotZero(p)) : p;
}

vec3 ddgiOctDecode(vec2 f)
{
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// Direction a given interior texel of an NxN octahedral map stands for.
vec3 ddgiTexelDir(ivec2 interior, int n)
{
    const vec2 uv = (vec2(interior) + 0.5) / float(n);
    return ddgiOctDecode(uv * 2.0 - 1.0);
}

// ---------------------------------------------------------------------------
//  Toroidal addressing
// ---------------------------------------------------------------------------
ivec3 ddgiSlotCoord(int slotIdx)
{
    const int dx = ddgi.dims.x, dy = ddgi.dims.y;
    return ivec3(slotIdx % dx, (slotIdx / dx) % dy, slotIdx / (dx * dy));
}

int ddgiSlotIndex(ivec3 slot)
{
    return slot.x + slot.y * ddgi.dims.x + slot.z * ddgi.dims.x * ddgi.dims.y;
}

// The absolute grid coordinate a slot currently stands for: the unique value
// congruent to `slot` modulo the grid size that lies inside the window.
ivec3 ddgiSlotToAbs(ivec3 slot, ivec3 windowBase)
{
    const ivec3 d = ddgi.dims.xyz;
    return windowBase + ((slot - windowBase) % d + d) % d;
}

ivec3 ddgiAbsToSlot(ivec3 absCoord)
{
    const ivec3 d = ddgi.dims.xyz;
    return (absCoord % d + d) % d;
}

// The lattice point itself — where a probe would stand with relocation off.
vec3 ddgiLatticePos(ivec3 absCoord, int c)
{
    return vec3(absCoord) * ddgiSpacing(c);
}

// ---------------------------------------------------------------------------
//  PROBE RELOCATION — where a probe's offset lives, and why it lives THERE
//
//  A probe on the bare lattice has no idea whether it stands in open air or a
//  metre inside a wall, and the buried ones are not a rare edge case: at 3 m
//  spacing every probe of a 4 m room is in masonry, which is exactly why the
//  interior went black and why 2 m spacing printed lattice blotches outdoors
//  (probe planes landing just under the terrain). Relocation lets each probe
//  slide inside its own cell, up to r_ddgi_reloc_max of the spacing, towards
//  whatever open space its own rays found.
//
//  ⭐ THE OFFSET IS STORED IN A STRIP AT THE BOTTOM OF THE IRRADIANCE ATLAS,
//  one RGBA16F texel per probe (xyz = metres, w = "this probe just jumped").
//  That is not a trick to save memory — it is what keeps this feature from
//  touching SIX descriptor sets. Every consumer of the field (the world, the
//  terrain, the grass, the trees, the final gather, the world radiance cache,
//  the froxel field, the debug view) already has this atlas bound, so putting
//  the offsets anywhere else would have meant a new binding, a new fallback and
//  a new rebind path in each of them. Here they cost one texelFetch and nothing
//  else changes.
//
//  ⚠ The strip sits BELOW every probe tile, so no bilinear tap inside a tile can
//  reach it (the one-texel border already keeps taps inside their own tile).
ivec2 ddgiOffsetTexel(int gprobe)
{
    return ivec2(gprobe % ddgi.reloc.y, ddgi.reloc.x + gprobe / ddgi.reloc.y);
}

// Where cascade c's probes live in the shared atlas: one flat run per cascade.
int ddgiGlobalProbe(int c, int slotIdx) { return c * ddgi.casc.y + slotIdx; }

// How much of cascade c's answer a world point is entitled to: 1 well inside,
// falling to 0 over a two-cell band at that cascade's boundary.
float ddgiCascadeFade(vec3 P, int c)
{
    const float s  = ddgiSpacing(c);
    const vec3  lo = vec3(ddgi.base[c].xyz) * s;
    const vec3  hi = lo + vec3(ddgi.dims.xyz - ivec3(1)) * s;
    const vec3  d  = min(P - lo, hi - P);
    return clamp(min(min(d.x, d.y), d.z) / max(s * 2.0, 1e-3), 0.0, 1.0);
}

// How much of the VOLUME's answer a world point is entitled to — i.e. the fade of
// the outermost cascade, where the world finally runs out.
//
// ⚠ THIS IS NOT COSMETIC. ddgiSampleIrradiance returns BLACK outside the window
// (it refuses to read a wrapped-around stranger, which is right), so a consumer
// that replaces its sky fill with the lookup and does not fade would paint the
// whole world beyond the volume pitch black — and the boundary would sweep across
// the frame as the player walks. Every consumer must lerp back to its own analytic
// ambient by this factor.
float ddgiVolumeFade(vec3 P)
{
    return ddgiCascadeFade(P, ddgi.casc.x - 1);
}

// True when this slot did NOT exist in the window as of this cascade's PREVIOUS
// UPDATE, i.e. it scrolled in and carries a stranger's history.
bool ddgiSlotIsNew(ivec3 absCoord, int c)
{
    const ivec3 rel = absCoord - ddgi.prevBase[c].xyz;
    return any(lessThan(rel, ivec3(0))) || any(greaterThanEqual(rel, ddgi.dims.xyz));
}

// ---------------------------------------------------------------------------
//  Ray directions: spherical Fibonacci, rotated per frame.
//
//  The rotation is what makes 32 rays behave like far more over time — without it
//  every frame would re-measure the SAME 32 directions and the hysteresis would
//  converge to a fixed, structurally biased answer instead of the true integral.
// ---------------------------------------------------------------------------
vec3 ddgiQuatRotate(vec4 q, vec3 v)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec3 ddgiFibonacciDir(int i, int n)
{
    const float PHI = 1.6180339887498948482;
    const float phi = 2.0 * 3.14159265 * fract(float(i) * (PHI - 1.0));
    const float cz  = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    const float sz  = sqrt(clamp(1.0 - cz * cz, 0.0, 1.0));
    return vec3(cos(phi) * sz, sin(phi) * sz, cz);
}

vec3 ddgiRayDir(int i, int n)
{
    return ddgiQuatRotate(ddgi.rayRot, ddgiFibonacciDir(i, n));
}

// FIXED RAYS (RTXGI's RTXGI_DDGI_NUM_FIXED_RAYS). Relocation and classification
// decide "is this probe buried" from a COUNT of short/backface hits; with the set
// rotated every update that count is re-sampled every update, a probe near the
// threshold flips between buried and not, and every flip is a "jumped" flag that
// throws its history away. So rays [0, F) are one set that never rotates and is
// never blended — a deterministic answer for a deterministic position — and the
// blends integrate only [F, dims.w), which is exactly the old rotated set.
int ddgiFixedRays()   { return max(ddgi.fix.z, 0); }
int ddgiBlendRays()   { return ddgi.dims.w - ddgiFixedRays(); }

// Direction of ray i of a probe, in the combined [fixed | rotated] layout. With
// F = 0 this is ddgiRayDir(i, dims.w), bit for bit.
vec3 ddgiProbeRayDir(int i)
{
    const int f = ddgiFixedRays();
    if (i < f) return ddgiFibonacciDir(i, f);
    return ddgiRayDir(i - f, ddgi.dims.w - f);
}

// Classification: does this update skip the probe's rotated rays and its blend?
// Inactive, and not this probe's turn to refresh. The refresh is staggered by
// probe index so each update pays for 1/period of the inactive probes, not all of
// them on one frame. Uniform per probe (per workgroup in the blends).
bool ddgiSkipInactive(int probe, int flags)
{
    if (ddgi.classify.x == 0 || (flags & DDGI_FLAG_INACTIVE) == 0) return false;
    const int period = max(ddgi.classify.y, 1);
    return ((ddgi.classify.z + probe) % period) != 0;
}

// The hysteresis an inactive probe blends with on its refresh update. It was not
// measured for period-1 updates, so one update carries period updates' worth of
// change: h^period is what a probe updated every time with this same measurement
// would have converged to. 1 = not classified, the plain h.
float ddgiRefreshHysteresis(float h, int flags)
{
    if (ddgi.classify.x == 0 || (flags & DDGI_FLAG_INACTIVE) == 0) return h;
    return pow(h, float(max(ddgi.classify.y, 1)));
}

#ifdef DDGI_SAMPLE
layout(set = DDGI_SET, binding = DDGI_B_IRR) uniform sampler2D ddgiIrrTex;
layout(set = DDGI_SET, binding = DDGI_B_DEP) uniform sampler2D ddgiDepTex;

// ⚠ COMPUTE CANNOT CALL texture(): implicit LOD needs derivatives, which only the
// fragment stage has. The probe atlases are SINGLE-MIP, so implicit LOD always
// resolves to level 0 anyway — textureLod(...,0.0) is the same fetch, not an
// approximation of it. Define DDGI_COMPUTE to get the compute-legal form; without
// it this expands to exactly the call that was here, so no shipping .spv moves.
#ifdef DDGI_COMPUTE
  #define DDGI_TEX(s, uv) textureLod(s, uv, 0.0)
#else
  #define DDGI_TEX(s, uv) texture(s, uv)
#endif

// The probe's displacement from its lattice point, in metres. Zero — and free,
// since the branch is uniform across the whole dispatch — while r_ddgi_reloc is
// off, which is what makes this a feature that can be A/B'd against itself.
vec3 ddgiProbeOffset(int gprobe)
{
    if (ddgi.reloc.z == 0) return vec3(0.0);
    return texelFetch(ddgiIrrTex, ddgiOffsetTexel(gprobe), 0).xyz;
}

// The whole strip texel: xyz = offset, w = DDGI_FLAG_* bits. Zeroes while
// relocation is off (the strip is not maintained then).
vec4 ddgiProbeState(int gprobe)
{
    if (ddgi.reloc.z == 0) return vec4(0.0);
    return texelFetch(ddgiIrrTex, ddgiOffsetTexel(gprobe), 0);
}

// Where the probe ACTUALLY stands. Every consumer must use this rather than the
// lattice: the distance moments in the depth atlas were measured from here, so a
// reader that assumed the lattice point would run the Chebyshev test against
// distances taken from somewhere else — up to half a cell away — and start
// rejecting probes that can see the surface perfectly well.
vec3 ddgiProbePos(ivec3 absCoord, int c, int gprobe)
{
    return ddgiLatticePos(absCoord, c) + ddgiProbeOffset(gprobe);
}

// UV of a direction inside one probe's tile. The +1 skips the border and the
// *N keeps the sample strictly inside the interior, which is what makes the
// border texels do their job instead of being sampled as data.
vec2 ddgiTileUV(int slotIdx, vec3 dir, int n, vec2 atlasSize)
{
    const int   ppr  = ddgi.layout_.x;
    const ivec2 tile = ivec2(slotIdx % ppr, slotIdx / ppr);
    const vec2  oct  = ddgiOctEncode(normalize(dir)) * 0.5 + 0.5;   // [0,1]
    const vec2  texel = vec2(tile * (n + 2)) + 1.0 + oct * float(n);
    return texel / atlasSize;
}

// Irradiance arriving at a surface, from the eight probes around it.
//
// Three weights multiply together, and each exists to kill a specific artefact:
//   * trilinear      — smooth interpolation between probes;
//   * wrap/backface  — a probe BEHIND the surface has nothing useful to say
//                      about its front, and would leak light through walls;
//   * Chebyshev      — the statistical visibility test from the distance moments.
//                      This is the one that makes DDGI different from an
//                      irradiance-volume-with-leaks: a probe on the far side of a
//                      wall reports a mean distance much shorter than the distance
//                      to the shaded point, and gets weighted out.
vec3 ddgiSampleCascade(int c, vec3 P, vec3 N, vec3 V)
{
    const float s = ddgiSpacing(c);

    // Bias the lookup off the surface: probes live on a lattice that has no idea
    // where the geometry is, and a sample taken exactly ON a surface sits half
    // inside it. Along the normal AND along the view, as RTXGI does.
    // ⚠ The biases are in METRES and are doubled per cascade, not multiplied by the
    // spacing: at cascade 0 that reproduces the tuned value exactly, and a coarser
    // cascade steps proportionally further out for the same reason.
    const float bs     = float(1 << c);
    const vec3  biased = P + N * (ddgi.grid.z * bs) + V * (ddgi.params.y * bs);

    const vec3  gf      = biased / s;
    const ivec3 baseAbs = ivec3(floor(gf));
    const vec3  alpha   = clamp(gf - vec3(baseAbs), 0.0, 1.0);

    const vec2 irrSize = vec2(textureSize(ddgiIrrTex, 0));
    const vec2 depSize = vec2(textureSize(ddgiDepTex, 0));

    vec3  sumIrr = vec3(0.0);
    float sumW   = 0.0;
    // The best Chebyshev visibility ANY consulted probe could claim for this point.
    // An unoccluded probe (bDist <= mean) skips the test and scores 1; a rejected
    // one scores its sharpened ratio, 1e-4..1e-2 deep behind a wall. See the
    // confidence gate after the loop for why this has to be tracked at all.
    float vis    = 0.0;

    for (int i = 0; i < 8; ++i) {
        const ivec3 off  = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const ivec3 absC = baseAbs + off;

        // Outside the window there is no probe at all — skipping keeps the volume's
        // edge honest instead of reading a wrapped-around stranger.
        const ivec3 rel = absC - ddgi.base[c].xyz;
        if (any(lessThan(rel, ivec3(0))) || any(greaterThanEqual(rel, ddgi.dims.xyz))) continue;

        const int  slot = ddgiGlobalProbe(c, ddgiSlotIndex(ddgiAbsToSlot(absC)));
        // ⚠ THE TRILINEAR WEIGHT STAYS ON THE LATTICE, the other two weights follow
        // the relocated probe. `alpha` above is what makes eight probes partition
        // unity, and it is derived from the cell the point falls in; recomputing it
        // from displaced positions would leave the eight weights not summing to one
        // and print the cell structure back into the image. The wrap term and the
        // Chebyshev test, on the other hand, ask "what can THIS probe see from
        // where it is", so they get the true position. Same split RTXGI uses.
        // One fetch for the offset AND the flags (ddgiProbePos would fetch the
        // same texel for the offset alone).
        const vec4 st   = ddgiProbeState(slot);

        // r_ddgi_skip_buried: a probe relocation could not get out of the solid
        // measured the inside of a wall. Its tile is not a weak opinion to be
        // weighted down, it is no opinion — RTXGI drops such probes from the
        // interpolation outright. Skipping also keeps it out of `vis`, so a point
        // whose only "survivor" was a buried probe falls to the confidence gate.
        if (ddgi.fix.w != 0 && (int(round(st.w)) & DDGI_FLAG_BURIED) != 0) continue;

        const vec3 pp   = ddgiLatticePos(absC, c) + st.xyz;

        const vec3  toProbe  = pp - P;
        const float distP    = length(toProbe);
        const vec3  dirP     = (distP > 1e-6) ? (toProbe / distP) : N;

        const vec3  tri = mix(1.0 - alpha, alpha, vec3(off));
        // ⚠ r_ddgi_crush_tri — WHERE THE TRILINEAR WEIGHT ENTERS. The crush below
        // is a curve on "how much does this probe's VISIBILITY trust it". Applied
        // to the product with the trilinear weight (the old order), it also bends
        // the interpolation kernel: a far-corner probe with tri = 0.05 is crushed
        // cubically however well it sees the point, the eight weights stop being a
        // trilinear blend and pull toward the nearest probe, and the lattice shows
        // in the image. RTXGI crushes wrap x Chebyshev alone and multiplies by
        // max(tri, 0.001) afterwards; that is what fix.y selects.
        const bool rtxgiOrder = ddgi.fix.y != 0;
        float w = rtxgiOrder ? 1.0 : tri.x * tri.y * tri.z;

        // Wrap shading: smooth, and never exactly zero, so a surface whose probes
        // are all slightly behind it still gets light instead of going black.
        const float wrap = (dot(dirP, N) + 1.0) * 0.5;
        w *= (wrap * wrap) + 0.2;

        // Chebyshev / variance shadow test against the probe's distance moments.
        const vec3  bDir  = biased - pp;
        const float bDist = length(bDir);
        float see = 1.0;   // what THIS probe claims: 1 unless the moments object
        if (bDist > 1e-6) {
            const vec2  md   = DDGI_TEX(ddgiDepTex, ddgiTileUV(slot, bDir, DDGI_DEP_N, depSize)).rg;
            const float mean = md.x;
            if (bDist > mean) {
                const float variance = abs(mean * mean - md.y);
                const float d        = bDist - mean;
                float cheb = variance / (variance + d * d);
                cheb = cheb * cheb * cheb;                 // sharpen: the raw ratio leaks
                see  = cheb;
                w *= max(cheb, 0.05);
            }
        }
        vis = max(vis, see);

        // Crush the tail: weights near zero carry no information but do carry
        // their probe's noise, and summing eight of them is how a wall bleeds.
        const float crush = 0.2;
        if (w < crush) w *= (w * w) / (crush * crush);
        w = max(w, 1e-8);
        if (rtxgiOrder) {
            const vec3 t = max(tri, vec3(0.001));
            w *= t.x * t.y * t.z;
        }

        sumIrr += DDGI_TEX(ddgiIrrTex, ddgiTileUV(slot, N, DDGI_IRR_N, irrSize)).rgb * w;
        sumW   += w;
    }

    if (sumW <= 1e-7) return vec3(0.0);
    vec3 irr = sumIrr / sumW;

    // ⚠ THE CONFIDENCE GATE (r_ddgi_conf, rides in ddgi.indoor.z; 0 = off, and the
    // old arithmetic bit-exact). The floors above — 0.2 in the wrap, 0.05 in the
    // Chebyshev — exist so ONE bad probe cannot black-hole a lookup. But when ALL
    // EIGHT probes are rejected, the sumIrr/sumW normalisation rescales those
    // floors back to full brightness, and the answer becomes "whichever reject was
    // crushed the LEAST". At the far end of a corridor that winner is the probe
    // standing OUTSIDE the building: the in-wall probes have near-zero variance
    // and are crushed to nothing, the open-air one keeps a large variance and a
    // survivable ratio — and the corridor's end GLOWS with open sky (the measured
    // symptom this gate was written against, 27-08).
    //
    // `vis` separates the two regimes by orders of magnitude: any probe that
    // plausibly sees the point scores ~1 (an unoccluded one skips the test
    // entirely), while "every probe rejected" tops out at 1e-4..1e-2. Below the
    // threshold the honest answer is darkness — "enclosed, no data" — not a
    // confident average of eight rejects. The consumers already treat black as a
    // real answer (it is what the volume returns outside its window), and the
    // per-pixel gather / WRC still add whatever light genuinely reaches there.
    if (ddgi.indoor.z > 0.0) irr *= smoothstep(0.0, ddgi.indoor.z, vis);
    return irr;
}

// Irradiance from the FINEST cascade that still covers this point, cross-faded into
// the next one where it runs out.
//
// The cross-fade is not decoration: nested cascades differ in density, so a hard
// switch prints a ring on the ground centred on the player and moving with them —
// the most distracting artefact a camera-centred structure can produce. Only points
// inside a two-cell shell pay for the second lookup; everything else takes one.
vec3 ddgiSampleIrradiance(vec3 P, vec3 N, vec3 V)
{
    const int nc = ddgi.casc.x;
    for (int c = 0; c < nc; ++c) {
        const float f = ddgiCascadeFade(P, c);
        if (f <= 0.0) continue;
        const vec3 fine = ddgiSampleCascade(c, P, N, V);
        if (f >= 0.999 || c + 1 >= nc) return fine;
        vec3 coarse = ddgiSampleCascade(c + 1, P, N, V);
        // r_ddgi_edge_clamp (edge.x): cap the coarse cascade's energy at k x the
        // fine one's before the mix. The fine cascade COVERS this point (f > 0) and
        // its answer is post-conf-gate, so "fine is dark" is a verdict, not missing
        // data; a coarse lattice too sparse to hold a probe inside the room must
        // not overrule it. Scaled by luminance, not min() per channel, so the
        // coarse hue survives. See the block comment at `edge`.
        const float k = ddgi.edge.x;
        if (k > 0.0) {
            const vec3  lw   = vec3(0.2126, 0.7152, 0.0722);
            const float cap  = k * dot(fine, lw);
            const float lumC = dot(coarse, lw);
            if (lumC > cap) coarse *= cap / max(lumC, 1e-6);
        }
        return mix(coarse, fine, f);
    }
    return vec3(0.0);
}
#endif  // DDGI_SAMPLE

#endif  // DDGI_COMMON_GLSL
