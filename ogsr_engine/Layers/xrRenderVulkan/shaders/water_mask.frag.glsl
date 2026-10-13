#version 450
// xrRenderVulkan — WATER POOL MASK, fragment stage.
//
// Writes the surface height so the sim can tell one pool from another. Texels
// never covered keep the clear value ("dry").
//
// ⚠ THE HARD PART. Level water is not modelled as one mesh per puddle: it is a
// few ENORMOUS flat sheets, and what you see as "several puddles on the cellar
// floor" is one sheet poking through an uneven floor. The surface shader hides
// the rest with a manual depth test, so the eye sees separate pools while the
// GEOMETRY is a single slab — and a mask built from geometry alone therefore
// declared the whole floor one body of water, which is why the ripple still
// crossed between puddles after the first attempt.
//
// So the mask asks the same question the eye does — but NOT "is the water above
// the floor". That was the first attempt and it went dark: the height map holds
// the NEAREST SURFACE SEEN FROM ABOVE, and in a cellar that is the ceiling, not
// the floor under the water. Every texel indoors came out "water below the
// floor", the whole field went dry and no splash made a wave at all.
//
// The question that actually separates visible water from hidden water is
// CLEARANCE: is there open space above it? A sheet passing under a floor has a
// surface lying directly on it; a pool in a room has metres of air over it, and
// so does open water. Compare against the nearest surface ABOVE only — never
// against terrain BELOW, or every pond outdoors would mask itself off.
//
// WATER_MASK_SURF adds the SECOND target the shore wetness runs on. See below.
#extension GL_GOOGLE_include_directive : require
#define WATER_NO_RIPPLE 1          // the sim holds the interactive field itself
// ⭐ THE SPECTRAL FIELD IS READ HERE TOO (bindings 6/7 were added to this pass's
// own little set for it). It shipped without them for one day and the cost was
// exactly what you would predict: the wet band tracked the ANALYTIC swell while
// the water you could see was displaced by the spectral one, so the dark strip
// and the waterline that made it disagreed by a few centimetres. Same rule as
// ever on this surface — one wave, every consumer.
#define WATER_SHORE_NO_MAP 1       // this pass WRITES that map; it cannot sample it
#include "water_common.glsl"
#include "water_shore.glsl"

layout(set = 1, binding = 13) uniform sampler2D uGround;
// Shelter asks the RAIN map, not the ground map, because that is the one the
// water surface itself asks — and the two must agree about where the wind is,
// or the wet band would breathe in a cellar where the water is dead flat.
layout(set = 1, binding = 9)  uniform sampler2D uRainMap;

layout(push_constant) uniform PC {
    vec4 tile;
    mat4 rainVP;
    vec4 basin;   // x = fetch (m), y = metres per unit ortho depth, z = ground reach (m)
} pc;

layout(location = 0) in  vec3  vWorld;
layout(location = 0) out float outY;
#ifdef WATER_MASK_SURF
// SECOND TARGET — the two facts the shore wetness needs and cannot get anywhere
// else, both per tile texel:
//
//   .r = the LIVE water surface: the still sheet plus the same swash the water
//        shader runs its shoreline on. Wetness has to follow the wave that is
//        actually drawn, or the dark band and the visible waterline disagree.
//   .g = the GROUND in this column, from the top-down height map, or "dry" when
//        that map cannot answer for it.
//
// ⚠ Written even where the mask above DISCARDS. That is the whole point: the
// clearance rule throws away exactly the strip where the bank stands a handful
// of centimetres out of the water — which is the swash zone, the only place the
// run-up is ever visible. So the discard became "write dry to target 0" (identical
// under a MAX blend against a -10000 clear) and this target keeps its data.
layout(location = 1) out vec4  outSurf;
#endif

const float kDry = -10000.0;

// Ortho depth spans 349 m over [0,1] (near 1, far 350), so this is ~35 cm of
// headroom. Water with less than that above it is under a floor and invisible.
// Erring low merges puddles again; erring high punches holes in water that
// merely runs beneath a plank — and a hole reads as a wall to the wave.
const float kClearance = 0.001;

void main()
{
    const float still = vWorld.y;
    bool dry = false;

    vec3 n  = (pc.rainVP * vec4(vWorld, 1.0)).xyz;
    vec2 uv = n.xy * 0.5 + 0.5;
    uv.y    = 1.0 - uv.y;
    const bool inMap = (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && n.z > 0.0 && n.z < 1.0);
    float topZ = 1.0;
    // Outside the ground map there is nothing to compare against: keep the
    // water rather than punching a hole in the field.
    if (inMap) {
        topZ = textureLod(uGround, uv, 0.0).r;
        // Bigger depth = lower, so `topZ < n.z` means that surface is ABOVE the
        // water. Only then is it a lid: terrain below the surface (every pond
        // outdoors) must not count, or the mask would erase the water it sits in.
        bool lid = topZ < n.z;
        // tile.w = STRICT mode, used by the level-wide map. The clearance test
        // above asks "is something lying ON this water" — a 35 cm rule written for
        // a sheet running under a cellar floor. It says nothing about a sheet
        // buried TWO METRES under a hillside, which sails through with clearance
        // to spare and comes out marked as standing water above the river.
        //
        // For the level map the question is simply "is there ground over it at
        // all". That does cost the indoor case (a cellar's roof counts as ground,
        // so its pools drop out) — acceptable, because indoor water is covered by
        // the ripple tile, whose test is a real downward ray, not a height map.
        if (pc.tile.w > 0.5) { if (lid) dry = true; }
        else if (lid && (n.z - topZ) < kClearance) dry = true;
    }
    outY = dry ? kDry : still;

#ifdef WATER_MASK_SURF
    // ---- the ground --------------------------------------------------------
    // Decoded FIRST, because the break below is a function of the depth and the
    // depth is `still - ground`. The top-down height map answers this directly and
    // per texel — no ray, no budget, no latency — but only for what it can SEE
    // from above, so its answer is accepted only inside the band the water can
    // plausibly reach. Anything higher is a hillside over a buried sheet or a
    // cellar ceiling, and reading either as "the ground" would soak a hill or dry
    // out a basement. The sim falls back to its own downward ray when this comes
    // back dry.
    //
    // topZ == 1 is the map's CLEAR value: nothing was drawn in that texel (or the
    // map was never rendered). That is "no answer", not "the ground is at infinity"
    // — taken literally it puts the ground below every sheet on the level and
    // soaks the world.
    float ground = kDry;
    if (inMap && topZ < 0.999) {
        const float gy = still + (n.z - topZ) * pc.basin.y;
        if (gy <= still + pc.basin.z) ground = gy;
    }

    // ---- the live surface --------------------------------------------------
    // The swell ALONE — never the interactive field, because the sim adds its own
    // height downstream and r_wtr_sim_height is a look knob people set to 4.
    //
    // ⚠⚠ AND NEVER r_wtr_swash. It is tempting: water.frag computes exactly
    // `field.w * W.p10.x * energy` a few lines from here and calls it "swash", so
    // copying it looks like agreeing with the surface. It is not a HEIGHT. It is
    // added to a DEPTH to drive an alpha fade over r_wtr_shore (22 cm), i.e. a
    // look multiplier for how far the transparent edge breathes — and at its
    // default of 2.2 it turns a 19 cm wave into a 42 cm one. That is the half a
    // metre of "wet" bank standing above the water with nothing to explain it.
    // The surface is displaced by the swell and by nothing else; so is the mark.
    //
    // ⚠ No distance LOD either, for a subtler reason: the LOD exists to stop the
    // short octaves shimmering, so it is a function of where the CAMERA is. This
    // map is a persistent record of what the world did, and a world record that
    // moves when you walk toward it is not a record. Evaluated at full detail, it
    // can only overshoot the drawn wave by the few centimetres the far-field LOD
    // was suppressing anyway.
    // ⚠ THE SAME FETCH THE SURFACE IS DRAWN WITH, or this record and the water
    // that made it disagree — the defect this pass keeps being about. The value
    // pushed with the draw is the span of the MESH (one visual per level), so a
    // well 2 m across was told it sat in a 108 m basin: the live surface written
    // here climbed 37 cm above a still sheet in 22 cm of water, and the contact
    // test soaked a ring of dry yard that then dried and re-wetted with every
    // crest. Measured per texel, that well reads 2 m and the ring never happens.
    //
    // The map is one frame old here (this pass runs before the step that builds
    // it) and it is indexed with THIS frame's tile origin. That is a mismatch of
    // one frame of camera drift — under 10 cm at a run, against a map at 0.5 m
    // per texel, on a quantity that only feeds a smoothstep.
    const float fetch = waterLocalFetch(vWorld.xz, pc.basin.x);
    const float calm  = waterShelter(uRainMap, vWorld);
    const float gust  = waterGust(vWorld.xz, W.p0.x, W.p1.xy);
    // The SPECTRAL field, on the identical terms water_common's waterField uses:
    // what it takes over, the analytic swell gives up (1 - fmix), and its height
    // is added on top. Not a copy of that code for its own sake — it is the same
    // two calls in the same order, and any drift between them is exactly the
    // "wet band does not match the wave" defect this pass keeps being about.
    vec2  fsl;
    float fmix;
    const vec4  fft   = waterFFTAll(vWorld.xz, fetch, calm, fsl, fmix);
    const vec3  swell = waterSwell(vWorld.xz, W.p0.x, W.p0.y * gust * calm * (1.0 - fmix), W.p0.z, W.p1.xy, 1.0, fetch);
    // ⚠ The swell is scaled by the DISPLACEMENT gain the tessellation stage
    // actually applies (water.tese: wp.y += f.x * W.p5.w * fade²). It is ZERO when
    // there are no tessellation stages — the surface is then drawn dead FLAT, and
    // a wet band riding a 19 cm wave nobody can see is the most confusing kind of
    // wrong. The fade² is left out on purpose: like the LOD, it is a function of
    // the camera, and this map is a record of the world.
    //
    // The SWASH is added on top and is not scaled by any of that: the run-up is
    // not geometry, it is a thin sheet the surface shader draws over the sand by
    // offsetting its own depth fade. Whatever it covers, it wets — and it must be
    // the same function, on the same clock, or the dark strip and the water that
    // made it drift apart within seconds.
    const float energy = waterSwellEnergy(fetch) * calm;
    // THE BREAK, from the ground this very shader has just decoded. The depth it
    // needs is `still - ground`, which is one subtraction away here — so the map
    // that records what got wet is driven by the identical function that displaces
    // the crest and paints the whitewater. Three consumers, one wave.
    float swash;
    if (W.p11.w > 0.0 && ground > -9000.0)
        swash = waterShoreBreak(still - ground, W.p0.x, W.p11.z, W.p10.x, W.p10.w, W.p11.x).x;
    else
        swash = waterSwash(vWorld.xz, W.p0.x, W.p10.x, W.p10.w, W.p1.xy, energy).x;
    // ⚠ The spectral height goes INSIDE the displacement gain with the swell, not
    // beside it: the vertex stages apply W.p5.w to waterField's TOTAL (swell plus
    // spectral), so anything that scales one has to scale the other. Split them
    // and the record drifts from the geometry the moment r_wtr_disp is touched.
    const float wave = (swell.x + fft.y * W.p12.w) * W.p5.w;
    const float live = still + max(wave, swash);
    // .b = the STILL sheet level. The shore break is a function of the still water
    // DEPTH, and the pool mask cannot supply it: its clearance rule discards the
    // very strip where the bank stands a few centimetres out of the water — the
    // swash zone. This target is written there, so it is the only place the depth
    // is knowable across the whole shore.
    outSurf = vec4(live, ground, still, 0.0);
#endif
}
