#ifndef WRC_COMMON_GLSL
#define WRC_COMMON_GLSL
// ===========================================================================
//  xrRenderVulkan — WORLD RADIANCE CACHE: the spatial hash, and the one query
//  every consumer asks it. Cache #2 of idTech 8's chain, and the link this
//  renderer did not have.
// ===========================================================================
//  THE CHAIN, AND WHERE THE HOLE WAS
//  ---------------------------------
//  A ray that needs "how much light leaves the surface I just hit" has three
//  places to ask, cheapest and sharpest first:
//
//    1. LAST FRAME'S SCREEN — a pixel that already holds exactly that, fully
//       lit, at pixel resolution. Free, but only where the hit was on screen
//       and unoccluded (gather.comp's scacheLookup).
//    2. THIS FILE — a world-space hash of 25 cm cells holding the shaded
//       radiance of the patch, valid ANYWHERE the geometry is, behind the
//       camera included, and REUSED ACROSS FRAMES.
//    3. THE PROBE LATTICE — always an answer, at 3 m and low frequency.
//
//  Without 2, a miss in 1 fell straight to 3: every gather ray that pointed
//  away from the screen got the 3 m answer, and every probe ray had to shade
//  its hit from scratch, every frame, forever.
//
//  WHY THIS IS BOTH SHARPER AND CHEAPER — THE SAME PROPERTY, TWICE
//  ---------------------------------------------------------------
//  A cell is shaded ONCE and then read by every ray that lands in it, this
//  frame and for several frames after. That decouples the COST of shading from
//  the NUMBER of rays, which is the whole reason idTech 8's probes cost 0.08 ms
//  where ours cost 0.2-0.8: their probe ray does not shade, it looks up. The
//  same lookup is 12x finer than the lattice it replaces, so it is also the
//  answer to "the GI is smooth but it is not SHARP".
//
//  ⚠ IT IS A CACHE, SO IT IS ALWAYS ONE STEP BEHIND. A cell requested this
//  frame is shaded at the top of the NEXT one and read from the one after.
//  Nothing here ever blocks on that: an unshaded cell answers "no confidence"
//  and the caller falls back to the probe field exactly as it did before this
//  file existed. That is why turning the cache on can never produce a hole —
//  the worst case is the old behaviour.
//
//  ⚠ THE KEY CARRIES A NORMAL BUCKET, AND THAT IS NOT AN OPTIMISATION. Two
//  sides of a 10 cm wall are one 25 cm cell apart in POSITION. Hashing position
//  alone would let the lit outside of a shack feed the dark inside of it — a
//  light leak indistinguishable from a broken visibility test, and the single
//  most expensive class of bug this arc has already paid for once (the
//  Chebyshev test in ddgi_common exists for the same reason).
//
//  Requires the includer to define, before the #include:
//    WRC_SET, WRC_B_CONSTS, WRC_B_CELLS, WRC_B_LIST, WRC_B_CTRL
// ===========================================================================

layout(std140, set = WRC_SET, binding = WRC_B_CONSTS) uniform WrcConstsBlock {
    // xyz = camera world position (the LOD staircase is measured from it),
    // w = cell size of level 0 in metres.
    vec4  camPos;
    // x = metres of cell per metre of distance (the LOD slope; 0 = one level
    //     everywhere), y = highest level allowed, z = master weight (0 = the
    //     cache answers nothing and no ray touches it), w = the shade pass's
    //     EMA weight for a re-shade (1 = take the new value whole).
    vec4  params;
    // x = capacity - 1 (a power-of-two mask), y = frame index, z = cap on cells
    // shaded per frame, w = frames between re-shades of one cell.
    // ⚠ y is also the value written into `touch`, so it must be the SAME number
    // for every consumer in one frame — it is, they all read this block.
    uvec4 ctl;
    // x = probe chain length (slots searched on a collision), y = stats on,
    // z = debug view mode, w = sky-visibility rays per cell shade (0 = off).
    uvec4 ctl2;
    // x = how far a sky-visibility ray reaches (m). Beyond it a miss means
    // "nothing built" rather than "open sky", so this is medium-range hemisphere
    // occlusion — the same thing the baked lightmap's hemi channel carries.
    // y = what a sky-visibility ray that HITS contributes (r_wrc_skyvis_bounce):
    //     0 = nothing, the old binary occluder; 1 = whatever the cache already
    //     knows at the hit, i.e. a real second bounce.
    // z = r_wrc_sky_field: > 0.5 and the shade pass rolls NO rays of its own —
    //     the cell's ambient is a probe-field read (the idTech 8 cell: direct
    //     lights + previous frame's irradiance volumes). x/y are then unused —
    //     UNLESS w below is on, which brings the hemisphere back as a meter.
    //     > 1.5 (mode 2): cache-to-cache — a COVERED cell's ambient is its own
    //     hemisphere (sky through the openings + neighbour cells), the field
    //     never enters; open-sky cells keep the field read. x/y are live again.
    // w = r_wrc_field_clamp: > 0 puts an ENERGY CEILING on that field read —
    //     k x the cell's own measured injection (see wrc_shade.comp.glsl).
    //     0 = off, and the sky_field path is bit-for-bit what it was.
    vec4  params2;
    // x = r_wrc_ema_up: multiplier on the re-shade EMA weight when the new value
    //     is BRIGHTER than the stored one (1 = symmetric, the old arithmetic).
    //     See the asymmetric-EMA note at the store in wrc_shade.comp.glsl.
    // y/z/w reserved. Appended at the TAIL: every offset above is unchanged, so
    // consumers of this block that were not recompiled keep reading valid words.
    vec4  params3;
} wrc;

// 32 B, all scalars, so the std430 stride is exactly sizeof — no vec4 padding
// surprises of the kind that cost this renderer a day in the push-constant block.
//
// ⚠ THE RECORD'S WIDTH IS THE CACHE'S CAPACITY, AND THE FIRST VERSION WAS 48 B.
// Measured 22-08: 512k slots, ~1M queries a frame, and 10-15% of them LOST to full
// probe chains — the table was simply too small for the working set. idTech 8 fits
// its whole cell in 128 bits + a packed colour; the fat here was three floats of
// position and three of radiance. Normal and albedo now share one word, radiance
// is RGB9E5 (the format the idTech 8 talk explicitly asks every GPU to support),
// and the same 24 MB now holds half again as many cells.
//
// ⭐ THE FIELDS ARE GROUPED BY WHO WRITES THEM, and that is load-bearing: the
// requester owns touch/pos/surf, the shade pass owns rad/shaded, the eviction
// sweep owns key (and clears shaded). Two writers sharing one 32-bit word would
// need a read-modify-write, and a read-modify-write from two dispatches is a lost
// update — which is why normal and albedo may share a word (one writer) and touch
// and shaded may not (two).
struct WrcCell {
    uint  key;      // checksum of the cell key; 0 = free slot     [evict + the claim CAS]
    uint  touch;    // frame index when a ray last asked for this  [requester, atomic]
    float px, py, pz;   // the representative surface point: a real hit, not the
                        // cell centre — a cell straddling a wall would otherwise
                        // shade from INSIDE it                    [requester]
    uint  surf;     // oct 8:8 normal | RGB565 linear albedo       [requester]
    uint  rad;      // cached OUTGOING radiance as RGB9E5, in the units
                    // ddgi_trace's `radiance` holds BEFORE its skyColor.w scale
                    //                                             [shade pass]
    uint  shaded;   // frame index + 1 of the last shade; 0 = NEVER [shade + evict]
};

// ⚠ NOT `coherent`, deliberately. Every cross-invocation handshake here is an
// ATOMIC (device scope by construction); the plain fields are written by one
// invocation and read only by a LATER DISPATCH, which the pass's barrier makes
// visible. Marking the whole 25 MB buffer coherent would push the radiance reads
// — the hot path, one per ray — past the L1 for no correctness gained.
layout(std430, set = WRC_SET, binding = WRC_B_CELLS) buffer WrcCells { WrcCell wrcCell[]; };
layout(std430, set = WRC_SET, binding = WRC_B_LIST)  buffer WrcList  { uint    wrcList[]; };
layout(std430, set = WRC_SET, binding = WRC_B_CTRL)  buffer WrcCtrl {
    uint listCount;    // cells queued for shading (reset after each shade pass)
    uint overflow;     // queue appends that did not fit
    uint queries;      // ray queries               \.
    uint hits;         // ...answered from the cache | stats only, behind ctl2.y:
    uint inserts;      // ...that claimed a new cell | one atomic per ray is not
    uint lost;         // ...whose probe chain was full/
    // Hits the SCREEN cache would have overridden anyway, so this cache is never
    // even asked. Not a curiosity: it is the number that says how much of the
    // frame belongs to cache #1 and how much is this one's actual job.
    uint screenWins;
    // ⭐⭐⭐ HOW FAR THE CACHE'S ANSWER IS FROM THE ONE IT REPLACED, summed as
    // fixed point (1/1024) over deltaCount samples.
    //
    // This exists because two sessions were spent asking an EYE whether a cache
    // changes a frame, and the eye kept answering "no" — which is worthless
    // evidence either way. A cache that agrees with the lattice to within 2% is a
    // pure cost and should be deleted; one that disagrees by 50% is working and
    // the invisibility lives further downstream, in the four mechanisms that share
    // the fill light. Those are opposite conclusions and no screenshot separates
    // them. Behind the stats flag, so it costs nothing when it is not being read.
    uint deltaSum;
    uint deltaCount;
    uint shadedLast;   // cells the last shade pass processed (NOT reset per frame)
    // ---- r_wrc_field_clamp self-audit (appended at the TAIL: every offset above
    // is unchanged, so older .spv consumers of this block stay valid). Written by
    // the shade pass, cleared right after the readback copy each frame — the
    // clamp's whole subject is invisible geometry, so without numbers "clamp on"
    // and "clamp never fires" read identically (the relocation audit's argument).
    uint clampSeen;      // sky_field cells shaded with the clamp enabled
    uint clampCovered;   // ...that passed the solid-cover gate (hemisphere ran)
    uint clampBitten;    // ...where the ceiling actually cut the field term
    uint clampBudgetM;   // sum of budgets over covered cells, milli-units (each capped at 16)
    uint clampSpendM;    // sum of field spends over covered cells, milli-units (same cap)
} wrcCtrl;

// ---------------------------------------------------------------------------
//  Key: cell coordinate + level + which way the surface faces.
// ---------------------------------------------------------------------------
uint wrcMix(uint x)
{
    // Murmur-style finaliser (Wang/xxhash constants). Cheap and, unlike a plain
    // multiply-shift, it spreads the LOW bits — which is all we use, since the
    // slot index is a mask of the bottom log2(capacity).
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Six buckets, by dominant axis and sign. Coarse on purpose: finer buckets split
// a curved surface into cells that each see a fraction of the rays and so are
// re-shaded more often than they are read, which is the cache paying for itself
// twice over. Six is enough to separate the two faces of any wall.
uint wrcFaceBucket(vec3 n)
{
    const vec3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return n.x > 0.0 ? 0u : 1u;
    if (a.y >= a.z)               return n.y > 0.0 ? 2u : 3u;
    return n.z > 0.0 ? 4u : 5u;
}

uint wrcHash(ivec3 c, uint lvl, uint face, uint seed)
{
    uint h = wrcMix(seed ^ uint(c.x));
    h = wrcMix(h ^ uint(c.y));
    h = wrcMix(h ^ uint(c.z));
    return wrcMix(h ^ (lvl | (face << 4)));
}

// LOD: cells grow with distance so one cell stays roughly one constant patch of
// SCREEN, not of world. Without it the far half of the bubble allocates ten
// thousand cells per square metre of hillside, each serving one ray — a cache
// that costs more to fill than the shading it saves.
//
// ⚠⚠ THE FIRST DEFAULT MADE THIS WHOLE FUNCTION INERT, and the log said so
// without anyone reading it. With a slope of 0.0075 the base size wins until
// `d * 0.0075 > 0.25`, i.e. past **33 m** — and the gather's cutoff is 30. Every
// cell in the entire working set was therefore level 0, the finest possible, and
// the staircase existed only outside the region that uses it. The symptom was not
// "the LOD is coarse", it was "the table is full and 15% of queries are lost".
// ⭐ The gate against a repeat is a NUMBER, not a comment: vk_wrc logs the
// distance at which each level starts, so an inert ladder is visible on sight.
//
// ⚠ THE STAIRCASE IS MEASURED FROM THE CAMERA, so a cell's level changes as the
// player walks and the point re-hashes into a different slot. That is a cache
// miss, not an error: the new cell fills in one frame.
int wrcLevel(vec3 P)
{
    const float base = max(wrc.camPos.w, 0.01);
    const float d    = distance(P, wrc.camPos.xyz);
    const float want = max(base, d * wrc.params.x);
    return clamp(int(floor(log2(want / base))), 0, int(wrc.params.y));
}

float wrcCellSize(int lvl) { return max(wrc.camPos.w, 0.01) * exp2(float(lvl)); }

// ---------------------------------------------------------------------------
//  Packing
// ---------------------------------------------------------------------------
// Normal and albedo share ONE word: octahedral 8:8 in the low half, RGB565 in the
// high half. 8 bits per octahedral component is ~1.5 degrees, and the only things
// that read this normal are a cosine term and a 2 cm shadow-ray offset — neither
// can tell 1.5 degrees from 0.1. Albedo at 5-6 bits is a 3% step on a quantity
// that never exceeds 0.9 and is already fetched from a coarse mip.
uint wrcPackSurf(vec3 n, vec3 a)
{
    const float l1 = abs(n.x) + abs(n.y) + abs(n.z);
    vec2 p = n.xy / max(l1, 1e-8);
    if (n.z <= 0.0)
        p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    const uvec2 q = uvec2(round(clamp(p, -1.0, 1.0) * 127.0) + 128.0);
    const uvec3 c = uvec3(round(clamp(a, 0.0, 1.0) * vec3(31.0, 63.0, 31.0)));
    return (q.x & 0xFFu) | ((q.y & 0xFFu) << 8) | (c.x << 16) | (c.y << 21) | (c.z << 27);
}

vec3 wrcUnpackNrm(uint v)
{
    const vec2 p = (vec2(uvec2(v & 0xFFu, (v >> 8) & 0xFFu)) - 128.0) / 127.0;
    vec3 n = vec3(p.x, p.y, 1.0 - abs(p.x) - abs(p.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

vec3 wrcUnpackAlb(uint v)
{
    return vec3(uvec3((v >> 16) & 0x1Fu, (v >> 21) & 0x3Fu, (v >> 27) & 0x1Fu))
         / vec3(31.0, 63.0, 31.0);
}

// RGB9E5: three 9-bit mantissas over one shared 5-bit exponent. The idTech 8 talk
// asks out loud for this format and names the alternative's failure — R11G11B10F
// has no exponent to share, so it prints a green-yellow shift and cannot make a
// clean white. Here it is also simply the only 32-bit format with the DYNAMIC
// RANGE this needs: cached radiance runs from a sunlit wall to an unlit cellar.
uint wrcPackRad(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(65408.0));
    const float m = max(max(c.r, c.g), c.b);
    int e = (m > 1e-9) ? (int(floor(log2(m))) + 1) : -15;
    e = clamp(e, -15, 16);
    float denom = exp2(float(e - 9));
    // Rounding can push the largest mantissa to 512, which does not fit in 9 bits.
    // One exponent step is the whole fix, and skipping it wraps the channel to 0 —
    // a black speck in the brightest cell of the frame.
    if (int(floor(m / denom + 0.5)) >= 512) { e += 1; denom *= 2.0; }
    const uvec3 q = uvec3(clamp(floor(c / denom + 0.5), vec3(0.0), vec3(511.0)));
    return q.x | (q.y << 9) | (q.z << 18) | (uint(e + 15) << 27);
}

vec3 wrcUnpackRad(uint v)
{
    const float s = exp2(float(int(v >> 27) - 24));   // (e+15) -> e-9
    return vec3(uvec3(v & 0x1FFu, (v >> 9) & 0x1FFu, (v >> 18) & 0x1FFu)) * s;
}

// A colour per cell, for r_wrc_debug: the cache's own structure made visible.
// This is the view that tells "the cache is thin" from "the cache is off" in one
// glance — cell size, LOD rings and collision churn are all in it.
vec3 wrcDebugColor(uint checksum)
{
    const uint h = wrcMix(checksum);
    return vec3(float(h & 0xFFu), float((h >> 8) & 0xFFu), float((h >> 16) & 0xFFu)) * (1.0 / 255.0);
}

// ---------------------------------------------------------------------------
//  THE QUERY. Reads the cache AND registers the cell as wanted, in one pass over
//  the probe chain — the two cannot be separated without walking it twice, and
//  idTech 8 marks the cell from the same ray hit for the same reason.
//
//  Returns 0 when the caller must fall back (cache off, cell not yet shaded,
//  probe chain full). Never returns a partial answer: a half-filled cell is
//  indistinguishable from a wrong one at the call site.
// ---------------------------------------------------------------------------
float wrcQuery(vec3 P, vec3 N, vec3 albedo, out vec3 rad, out uint outKey)
{
    rad    = vec3(0.0);
    outKey = 0u;
    if (wrc.params.z <= 0.0) return 0.0;

    const bool stats = (wrc.ctl2.y != 0u);
    if (stats) atomicAdd(wrcCtrl.queries, 1u);

    const int   lvl  = wrcLevel(P);
    const float size = wrcCellSize(lvl);
    const ivec3 c    = ivec3(floor(P / size));
    const uint  face = wrcFaceBucket(N);

    const uint mask  = wrc.ctl.x;
    const uint frame = wrc.ctl.y;
    // Two independent hashes of the SAME key: one picks the slot, the other is
    // stored in it. A slot match is then a 1-in-4-billion coincidence rather than
    // a 1-in-capacity one, which at half a million slots is the difference
    // between "never" and "several times a frame".
    uint slot = wrcHash(c, uint(lvl), face, 0u) & mask;
    const uint checksum = wrcHash(c, uint(lvl), face, 0x9E3779B9u) | 1u;   // never 0: 0 means "free"

    const uint chain = max(wrc.ctl2.x, 1u);
    uint idx = 0u;
    bool found = false;
    bool fresh = false;
    for (uint i = 0u; i < chain; ++i) {
        idx = (slot + i) & mask;
        const uint k = wrcCell[idx].key;
        if (k == checksum) { found = true; break; }
        if (k == 0u) {
            // Claim it. The CAS is the only safe way in: two rays land in the same
            // free slot every frame, and a plain store would let both believe they
            // own it and one of them write its key over the other's.
            const uint prev = atomicCompSwap(wrcCell[idx].key, 0u, checksum);
            if (prev == 0u || prev == checksum) { found = true; fresh = (prev == 0u); break; }
            // Somebody else took it between the read and the swap — keep walking.
        }
    }
    if (!found) {
        // The chain was full of other keys. idTech 8 drops the result here too;
        // the caller simply uses the probe field, which is what it did before.
        if (stats) atomicAdd(wrcCtrl.lost, 1u);
        return 0.0;
    }
    outKey = checksum;
    if (fresh && stats) atomicAdd(wrcCtrl.inserts, 1u);

    // ---- register the cell as wanted -------------------------------------
    // ⚠ THE PLAIN READ FIRST IS NOT A MICRO-OPTIMISATION. Without it every ray
    // that hits a busy cell issues a device-scope atomic to the same address —
    // ~1M scattered atomics a frame, all but a few thousand of them redundant.
    // With it, exactly one invocation per cell per frame reaches the exchange.
    if (wrcCell[idx].touch != frame) {
        const uint old = atomicExchange(wrcCell[idx].touch, frame);
        if (old != frame) {
            // FIRST requester this frame, and the only invocation allowed to write
            // the descriptor — which is why these plain stores need no atomics.
            // Refreshed every frame on purpose: the newest hit is the best estimate
            // of where in the cell the surface actually is.
            wrcCell[idx].px   = P.x;
            wrcCell[idx].py   = P.y;
            wrcCell[idx].pz   = P.z;
            wrcCell[idx].surf = wrcPackSurf(N, albedo);

            // Queue it for shading if it has never been shaded, or if its answer
            // is old enough to be worth redoing. THIS is the amortisation: a cell
            // read by a thousand rays over eight frames is shaded once.
            const uint sh = wrcCell[idx].shaded;
            if (sh == 0u || (frame + 1u - sh) >= wrc.ctl.w) {
                const uint n = atomicAdd(wrcCtrl.listCount, 1u);
                if (n < wrc.ctl.z) wrcList[n] = idx;
                else               atomicAdd(wrcCtrl.overflow, 1u);
            }
        }
    }

    // ---- read ------------------------------------------------------------
    if (wrcCell[idx].shaded == 0u) return 0.0;   // claimed, not yet shaded — fall back
    rad = wrcUnpackRad(wrcCell[idx].rad);
    if (stats) atomicAdd(wrcCtrl.hits, 1u);
    return wrc.params.z;
}

// ---------------------------------------------------------------------------
//  READ-ONLY TWIN OF wrcQuery, FOR INSTRUMENTS — at one explicit level (the GI
//  pixel debugger bridges the LOD staircase with it: the staircase is measured
//  from the camera, so a surface's cells change level as the player moves, and
//  the cell it used a moment ago sits one level over). Finds the cell and
//  touches NOTHING: no claim, no touch, no shade queue, no stats — a debugger
//  that registered cells would corrupt the evidence it prints (the requester
//  owns pos/surf, and a dump pass has no material fetch to fill them honestly).
//
//  Stopping at a free slot is correct, not lazy: wrcQuery claims the FIRST free
//  slot of the chain, so a key that exists always sits at or before it.
//  ageOut = frames since the cell's last shade, -1 when there is no shaded
//  cell. Returns the cache weight, exactly as wrcQuery would.
// ---------------------------------------------------------------------------
float wrcPeekLvl(vec3 P, vec3 N, int lvl, out vec3 rad, out int ageOut)
{
    rad = vec3(0.0); ageOut = -1;
    if (wrc.params.z <= 0.0) return 0.0;

    const float size = wrcCellSize(lvl);
    const ivec3 c    = ivec3(floor(P / size));
    const uint  face = wrcFaceBucket(N);

    const uint mask     = wrc.ctl.x;
    const uint slot     = wrcHash(c, uint(lvl), face, 0u) & mask;
    const uint checksum = wrcHash(c, uint(lvl), face, 0x9E3779B9u) | 1u;

    const uint chain = max(wrc.ctl2.x, 1u);
    for (uint i = 0u; i < chain; ++i) {
        const uint idx = (slot + i) & mask;
        const uint k   = wrcCell[idx].key;
        if (k == 0u) return 0.0;                 // end of this key's possible chain
        if (k != checksum) continue;
        const uint sh = wrcCell[idx].shaded;
        if (sh == 0u) return 0.0;                // claimed, never shaded
        ageOut = int(wrc.ctl.y + 1u - sh);
        rad    = wrcUnpackRad(wrcCell[idx].rad);
        return wrc.params.z;
    }
    return 0.0;
}

#endif  // WRC_COMMON_GLSL
