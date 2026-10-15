#version 450
// xrRenderVulkan — INTERACTIVE RIPPLES. One step of the 2D wave equation on a
// player-centred tile, plus this frame's disturbances.
//
//   v += (mean(4 neighbours) - h) * c
//   v *= damping
//   h += v
//
// The classic explicit finite-difference scheme (the one every browser water
// demo uses, and the one the reference projects the look was asked for use).
// It is worth being clear about why it is here instead of more analytic waves:
// a sum of sines cannot PROPAGATE, REFLECT or INTERFERE, and above all it
// cannot REACT — a footstep or a bullet has nowhere to write. This field can.
//
// Two things differ from the pool demos it comes from:
//
//  * The tile FOLLOWS THE CAMERA. The origin is snapped to whole texels, so
//    the scroll is an exact integer shift and the previous frame's field can be
//    re-read at an offset with no resampling blur.
//  * The borders ABSORB. A pool wants reflective walls; a moving window across
//    an open level must not ring off its own edge, so the outer band is damped
//    toward zero.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rg16f) uniform readonly  image2D uSrc;   // .r height (m), .g velocity
layout(set = 0, binding = 1, rg16f) uniform writeonly image2D uDst;

// v: xy = world XZ, z = radius (m), w = strength (m)
// e: x = the SOURCE's own height (m); <= -1e5 means "no height test" (rain, which
//    lands on whatever surface is there). Without it a splat is a position in
//    PLAN VIEW only, and the sheet running a metre below a river bank is just as
//    splashable as the water you are standing in.
struct Splat { vec4 v; vec4 e; };
layout(std430, set = 0, binding = 2) readonly buffer Splats { Splat s[]; } splats;

// POOL MASK: water surface height per texel, `dryValue` where there is no water.
// The field is ONE sheet without it, so a splash in one puddle crossed the dry
// floor between them and came up in every other puddle in the tile — the wave
// equation had no idea where the water ended.
layout(set = 0, binding = 3, r32f) uniform readonly image2D uMask;

// LID: the lowest solid surface ABOVE the water in this texel. Level water is a
// few huge flat sheets, so most of a cellar's "floor" is that sheet running
// UNDER the ground, invisible — and the mask alone cannot see it, because from
// straight above the nearest surface over a cellar is the building's roof, not
// the floor. Headroom is what separates a puddle from a hidden sheet.
layout(set = 0, binding = 4, r32f) uniform readonly image2D uLid;

// WATER DEPTH in metres — the column between this surface and the bottom, from
// the same CDB pass that builds the lid, one ray DOWN instead of up. It is what
// turns the field from a drum skin into water: a wave's speed is sqrt(g*h).
layout(set = 0, binding = 5, r32f) uniform readonly image2D uDepth;

// SHORE WETNESS, read and written here: .r = the highest world Y this column has
// been wetted up to, .g = how wet it still is. The world shading samples it, so a
// bank or a wall standing in water is dark up to the waterline and dries after
// the water leaves — the one part of "water interacts with the environment" that
// no amount of surface shading can fake, because it needs MEMORY of contact.
// ⚠ SEPARATE src/dst, ping-ponged like the field. One image read at p = c + shift
// and written at c inside a single dispatch is a RACE: a neighbouring workgroup
// overwrites p before this one reads it. With shift == 0 (standing still) p == c
// and nothing can go wrong, which is why it only ever showed while WALKING — the
// map smeared along the direction of travel.
// ⚠ RG32F, not RG16F. .r is a WORLD HEIGHT, and a half float steps by 6 cm at
// y = 64 and 12 cm at y = 128 — the same quantum that made the mask R32F. With
// the waterline pinned to the wave to the centimetre, that quantisation is the
// whole error budget: it comes out as a wet edge that climbs the beach in stairs.
layout(set = 0, binding = 6, rg32f) uniform readonly  image2D uWetSrc;
layout(set = 0, binding = 7, rg32f) uniform writeonly image2D uWetDst;

// LIVE SURFACE + GROUND, rasterized by the pool-mask pass (water_mask.frag):
//   .r = the water surface as it is DRAWN this frame — still sheet + swash
//   .g = the ground in this column, or the dry sentinel where the height map
//        could not answer (a hillside over a buried sheet, a cellar ceiling)
// These two are what turns "wet up to the waterline plus 35 cm, everywhere the
// sheet reaches in plan view" into CONTACT: a column is wetted when the water
// that is actually drawn is at or above the ground that is actually there.
layout(set = 0, binding = 8, rgba32f) uniform readonly image2D uSurf;

layout(push_constant) uniform PC {
    ivec2 shift;      // texels the tile scrolled since last frame (exact)   @0
    vec2  origin;     // world XZ of texel (0,0) THIS frame                  @8
    float mPerTexel;  //                                                     @16
    float damping;    // per-step velocity retention (0.995 ~ long-lived)    @20
    float stiffness;  // wave speed factor; keep <= 0.5 for stability        @24
    int   splatCount; //                                                     @28
    int   usePools;   // 1 = mask bounds the field                           @32
    float dryValue;   //                                                     @36
    float shoreAbsorb;// per dry side, per step: 1 = reflecting rim          @40
    // ⚠⚠ SCALARS ONLY BELOW THIS LINE. A vec2 here takes 8-byte alignment
    // under std430 and silently slides every member after it 4 bytes past
    // where the C++ struct writes them. That is not hypothetical: `camXZ` used
    // to be a vec2 at @44 in C++ and @48 in the shader, so the shader read
    // (camZ, reach) as the player position and took `reach` from @56 — off the
    // END of the 56-byte range being pushed. The reach limiter was reading
    // undefined memory for its entire life. Check with spirv-dis after ANY
    // change here; the struct declaration cannot be trusted to tell you.
    float camX;       // player position                                     @44
    float camZ;       //                                                     @48
    float reach;      // metres of full-strength ripple (0 = unlimited)      @52
    int   depthOn;    // 1 = wave speed follows the depth map                @56
    float depthRef;   // depth at which the wave runs at full speed (m)      @60
    float bedFric;    // extra per-step damping as the depth goes to zero    @64
    float wetDry;     // shore wetness kept per step (0 = feature off)       @68
    float wetLift;    // capillary rise above the line the water touched (m) @72
    float depthMax;   // ray range; depth == this means NO BOTTOM WAS FOUND  @76
    float wetSimH;    // r_wtr_sim_height — the field's DRAWN displacement   @80
} pc;

// Two puddles are the same body of water only if they touch AND sit at the same
// level. The height test matters indoors: a cellar floor is a staircase of small
// pools, and in plan view they touch at the step even though no wave can cross it.
const float kSameLevel = 0.35;   // metres
const float kLidGap    = 0.35;   // headroom below which the water is under a floor
// How far a splash's SOURCE may be from the surface it claims to hit. Above:
// a foot in water is at or below the surface, so anything much higher is
// standing on the bank. Below: wading chest-deep is legitimate, a storey down
// is not.
const float kFootAbove = 0.30;
const float kFootBelow = 2.50;

// Is there visible water in this texel? Both halves matter: the sheet must be
// there at all, and it must not be buried under the ground.
bool wetAt(ivec2 t, out float y)
{
    y = imageLoad(uMask, t).r;
    if (y <= pc.dryValue + 1.0) return false;
    return (imageLoad(uLid, t).r - y) >= kLidGap;
}

void main()
{
    const ivec2 sz = imageSize(uDst);
    const ivec2 c  = ivec2(gl_GlobalInvocationID.xy);
    if (c.x >= sz.x || c.y >= sz.y) return;

    // Read the previous field at the position this texel held LAST frame.
    // Outside the old tile there is no history — that band starts at rest,
    // which is correct: it is water the window has only just uncovered.
    const ivec2 p = c + pc.shift;
    const vec2 wp = pc.origin + (vec2(c) + 0.5) * pc.mPerTexel;   // world XZ of this texel
    vec2 st = vec2(0.0);
    if (p.x >= 0 && p.y >= 0 && p.x < sz.x && p.y < sz.y)
        st = imageLoad(uSrc, p).rg;

    // The mask is only meaningful when Pass_Water actually rasterized it this
    // frame (usePools). Reading it otherwise is reading an undefined image, and
    // wetness built on that would be noise burned into the world shading.
    float yC = 0.0;
    const bool wetHere = (pc.usePools != 0) && wetAt(c, yC);

    // ---- shore wetness: CONTACT, not plan view -----------------------------
    // Updated BEFORE the dry-texel bail below, because drying out is precisely
    // what a texel with no water in it has to keep doing. Scrolls with the tile
    // like everything else, so the memory belongs to the WORLD position, not to
    // a screen or a texel index.
    //
    // What this used to be: "the sheet covers this texel in plan view, so wet
    // everything under its still surface plus 35 cm". That is a CONTOUR BAND. It
    // appears at full strength the instant a shore comes into view, it never
    // dries, and on a shallow beach 35 cm of height is metres of sand no wave has
    // ever reached. What it is now: the surface that is DRAWN this frame, tested
    // against the ground that is actually there. A crest runs up the sand and
    // wets it; the water draws back and that strip starts drying.
    {
        vec2 wprev = vec2(-10000.0, 0.0);
        if (p.x >= 0 && p.y >= 0 && p.x < sz.x && p.y < sz.y)
            wprev = imageLoad(uWetSrc, p).rg;

        if (pc.wetDry <= 0.0 || pc.usePools == 0) {
            // Feature off — but the destination STILL has to be written, or the
            // ping-pong hands back whatever this image held two steps ago.
            imageStore(uWetDst, c, vec4(wprev, 0.0, 0.0));
        } else {
            const vec2  sf    = imageLoad(uSurf, c).rg;
            const bool  haveS = (sf.x > pc.dryValue + 1.0);
            float gy = sf.y;

            // The height map could not answer for this column. Indoors it never
            // can — from above, the nearest surface over a cellar is the roof —
            // so fall back to the sim's own ray, which starts AT the water and
            // looks DOWN and therefore finds the floor under it.
            if (haveS && gy <= pc.dryValue + 1.0) {
                const float yMask = imageLoad(uMask, c).r;
                const float dep   = abs(imageLoad(uDepth, c).r);
                if (yMask > pc.dryValue + 1.0 && dep < pc.depthMax - 0.01) gy = yMask - dep;
            }
            // A ROCK OR A SNAG STANDING IN THE WATER is "roofed" — it does put
            // solid matter above the surface — so it must keep blocking waves.
            // But its submerged side is soaked, and the height map sees only its
            // top. vk_water_ripple marks these by NEGATING the depth (free: a
            // depth cannot be negative); take the water's own level as the ground
            // there, less a margin so a trough does not blink it dry.
            if (haveS && imageLoad(uDepth, c).r < 0.0) {
                const float yMask = imageLoad(uMask, c).r;
                if (yMask > pc.dryValue + 1.0) gy = min(gy, yMask - 0.25);
            }

            // The surface as DRAWN: the mask carries the still sheet plus the
            // swash of the long wave; the interactive field adds its own height
            // with the same gain the tessellation displaces by.
            const float surf  = sf.x + st.x * pc.wetSimH;
            const bool  known = (gy > pc.dryValue + 1.0);
            const bool  touch = haveS && known && (surf >= gy - 0.02);

            if (!haveS) {
                // NO DATA for this column this frame — the water pass only draws
                // what is in the frustum, so turning your back on a shore is not
                // evidence that it dried. Hold instead of decaying; the memory
                // scrolls out of the tile soon enough anyway.
                imageStore(uWetDst, c, vec4(wprev, 0.0, 0.0));
            } else if (touch) {
                // Soak up to the line the water reached, plus the little that
                // capillary action and spray carry above it.
                const float target = surf + pc.wetLift;
                // A HIGH-WATER MARK THAT NEVER COMES DOWN is wrong on anything
                // with height to it: one gust crest would leave a bank, a wall or
                // a piling dark to that line for as long as its foot stays in the
                // water. Rise instantly, fall on the same clock as the drying.
                wprev.r = (wprev.r < target) ? target : mix(wprev.r, target, 1.0 - pc.wetDry);
                wprev.g = 1.0;
                imageStore(uWetDst, c, vec4(wprev, 0.0, 0.0));
            } else {
                wprev.g *= pc.wetDry;                     // out of the water: dry out
                if (wprev.g < 0.002) wprev = vec2(-10000.0, 0.0);
                imageStore(uWetDst, c, vec4(wprev, 0.0, 0.0));
            }
        }
    }

    // Dry texels hold nothing: a splash near the rim must not leave a standing
    // bump on the floor, and the height here is what the shading samples.
    if (pc.usePools != 0 && !wetHere) { imageStore(uDst, c, vec4(0.0)); return; }

    float h = st.x, v = st.y;

    // A neighbour that is dry, or is water at a DIFFERENT level, is a wall: hand
    // back this texel's own height so the gradient across that edge is zero. That
    // reflects the wave back into the pool instead of letting it leak out — which
    // is what the rim of a puddle actually does.
    float hL = h, hR = h, hD = h, hU = h;
    int   dryN = 0;                   // how many sides of this texel are shore
    {
        ivec2 o[4]    = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
        float hN[4]   = float[4](h, h, h, h);
        for (int i = 0; i < 4; ++i) {
            ivec2 pn = p + o[i];
            // Off the TILE is not a wall — the window must keep absorbing there
            // (a reflecting window rings), so that edge stays at zero as before.
            if (pn.x < 0 || pn.y < 0 || pn.x >= sz.x || pn.y >= sz.y) { hN[i] = 0.0; continue; }
            if (pc.usePools != 0) {
                ivec2 cn = c + o[i];
                if (cn.x < 0 || cn.y < 0 || cn.x >= sz.x || cn.y >= sz.y) continue;
                // The mask belongs to THIS frame's tile (indexed by c) while the
                // field belongs to last frame's (indexed by p) — same world point
                // either way, which is exactly what the integer scroll buys.
                float yN;
                if (!wetAt(cn, yN) || abs(yN - yC) > kSameLevel) { ++dryN; continue; }
            }
            hN[i] = imageLoad(uSrc, pn).r;
        }
        hL = hN[0]; hR = hN[1]; hD = hN[2]; hU = hN[3];
    }
    // SHALLOW WATER. Wave speed is sqrt(g*h), and in this scheme c is
    // proportional to sqrt(stiffness) — so letting stiffness track the DEPTH
    // reproduces the real thing directly: a ripple slows as the bottom rises,
    // its crests swing round to face the shore, and it bunches up on the way in.
    //
    // It also retires a hack. shoreAbsorb existed to stop waves at a rim the sim
    // could not otherwise see; with a depth map the wave simply runs out of speed
    // where the water runs out of depth, which is what actually happens.
    float k   = pc.stiffness;
    float bed = 1.0;
    if (pc.depthOn != 0) {
        const float dep = abs(imageLoad(uDepth, c).r);   // sign carries the obstacle flag
        const float sh  = clamp(dep / max(pc.depthRef, 0.01), 0.0, 1.0);
        k  *= sh;                       // c ~ sqrt(k) ~ sqrt(depth)
        // Bottom friction. Without it a wave that has run out of speed in the
        // shallows does not die there — it FREEZES, leaving a standing dent at
        // the waterline that nothing ever clears.
        bed = mix(pc.bedFric, 1.0, sh);
    }
    v += ((hL + hR + hD + hU) * 0.25 - h) * k;
    v *= pc.damping * bed;
    h += v;

    // ABSORBING SHORE. A zero-gradient rim REFLECTS, and a body of water ringed
    // by reflectors never loses energy — one splash keeps bouncing until it has
    // filled the whole sheet with standing waves. That is what "I jump in one
    // puddle and every puddle ripples" actually looked like: level water is a
    // single 20 x 13 m slab surfacing in several places, so the far "puddles" are
    // the same pool, and every reflection sent the wave straight back into them.
    // Bleeding the rim instead lets the wave DIE at the shore, the way a ripple
    // dies in mud, and keeps the disturbance near where it was made.
    if (dryN > 0) {
        float a = pow(pc.shoreAbsorb, float(dryN));
        h *= a; v *= a;
    }

    // REACH. The honest finding from the audit is that a cellar's "puddles" are
    // ONE 20 x 13 m slab surfacing in several places, so a wave that travels is
    // entitled to appear in all of them — there is no boundary between them to
    // respect. Since that is not what the eye expects, bound the disturbance in
    // SPACE: full strength near the player, faded out past `reach`. Distances are
    // measured from the player because the tile is centred on them anyway.
    if (pc.reach > 0.0) {
        float d = distance(wp, vec2(pc.camX, pc.camZ));
        float k = 1.0 - smoothstep(pc.reach, pc.reach * 2.0, d);
        h *= k; v *= k;
    }

    // Absorbing border: an 8% band bled toward zero. Without it every wave that
    // reaches the window edge bounces back inward and the tile slowly fills
    // with standing-wave interference that has nothing to do with the world.
    vec2  n    = (vec2(c) + 0.5) / vec2(sz);
    vec2  e    = min(n, vec2(1.0) - n);
    float edge = smoothstep(0.0, 0.08, min(e.x, e.y));
    h *= edge;
    v *= edge;

    // ---- this frame's disturbances ----------------------------------------
    // Cosine bell: continuous value AND first derivative at the rim, so the
    // splat injects a wave rather than a discontinuity that rings for ever.
    for (int i = 0; i < pc.splatCount; ++i) {
        vec4  s = splats.s[i].v;
        float d = length(wp - s.xy) / max(s.z, 0.01);
        if (d >= 1.0) continue;
        // THE SOURCE ITSELF MUST BE IN WATER, and in THIS pool. The CPU-side
        // test can only compare a foot against each visual's bounding SPHERE,
        // and for a kilometre-wide water sheet that sphere is the whole level —
        // so walking on dry ground between two puddles fired splats the entire
        // time, and every ring landed in both puddles. Only the mask knows where
        // the water really is, so the gate belongs here.
        if (pc.usePools != 0) {
            ivec2 sc = ivec2(floor((s.xy - pc.origin) / pc.mPerTexel));
            if (sc.x < 0 || sc.y < 0 || sc.x >= sz.x || sc.y >= sz.y) continue;
            float ys;
            if (!wetAt(sc, ys)) continue;                      // splashing on dry land
            if (abs(ys - yC) > kSameLevel) continue;           // ...or into a pool at another level

            // AND THE SOURCE MUST BE AT THIS WATER'S LEVEL, not merely over it in
            // plan view. The CPU test can only compare a foot against the visual's
            // bounding SPHERE, and for a kilometre-wide sheet that centre means
            // nothing — so standing on a BANK a metre above the water passed it,
            // and the sheet running invisibly under that bank rippled from every
            // step. Only the mask knows the surface height here, so the test lives
            // here too.
            const float srcY = splats.s[i].e.x;
            if (srcY > -1e5) {
                if (srcY - ys > kFootAbove) continue;   // standing above it: on the bank
                if (ys - srcY > kFootBelow) continue;   // ...or far under it: another storey
            }
        }
        h += (0.5 - 0.5 * cos((1.0 - d) * 3.14159265)) * s.w;
    }

    imageStore(uDst, c, vec4(h, v, 0.0, 0.0));
}
