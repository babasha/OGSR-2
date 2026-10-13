// xrRenderVulkan — SHORE WETNESS lookup (binding 32, built by vk_water_ripple).
//
// The other way a surface gets wet. Everything else in this renderer is wet
// because of RAIN — which is why a river used to run past bone-dry reeds, a dry
// log and a dry bank at noon in July. This map is written by CONTACT: the ripple
// tile records, per world column, how high the water has reached and how long
// ago, so the wetness survives the water receding and then dries.
//
//   .r = highest world Y the water has wetted this column up to
//   .g = how wet it still is (decays with time)
//
// Split out of wetness.glsl on purpose: grass (detail.frag) and foliage
// (tree.frag) want this and nothing else from that file, which drags in puddle
// placement, the Surface Field and the whole rain reflection core.
#ifndef SHORE_WET_GLSL
#define SHORE_WET_GLSL

layout(set = ENV_SET, binding = 32) uniform sampler2D uShoreWet;

// LEVEL WATER MAP: the surface height of every sheet on the level, rasterized
// once and never scrolled. Binding 32 above is a MEMORY of recent contact and
// only exists in a 32 m window around the player — which made wetness a fact
// about where you were standing: a shore twenty metres off stayed bone dry until
// you walked up and watched it soak. This one is the standing fact: water is
// here, at this height, whether or not anyone is looking.
layout(set = ENV_SET, binding = 33) uniform sampler2D uWaterLevel;

// SUBMERGED. Not "near the water" — UNDER it. This half answers the one thing
// about wetness that needs no memory and no wave: ground below the still surface
// of a sheet has water standing on it, everywhere on the level, whether or not
// anyone has ever been there, and it does not dry because the water has not gone.
//
// ⚠ It used to carry a LIFT as well — a damp band r_wtr_wet_lift metres ABOVE the
// line, so that the visible strip on the bank did not depend on the 64 m tile. But
// a band defined by HEIGHT is a contour line: on a beach at 1:10, 35 cm of lift is
// three and a half metres of "wet" sand that no wave has been near, painted in the
// instant the shore comes into view and never drying. That strip is exactly what
// the tile now measures properly, so this half hands it back and keeps only what
// it can state as fact.
float shoreWetnessStatic(vec3 wp)
{
    if (L.waterlvl.z <= 0.0) return 0.0;
    const float lift = L.waterlvl.w;
    vec2 uv = (wp.xz - L.waterlvl.xy) * L.waterlvl.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;
    // POINT sampling: dry texels hold a huge negative sentinel and interpolating
    // it against a real height invents a waterline that is not there. The band
    // is drawn at full pixel resolution by the height test below anyway, so the
    // map's own coarseness never shows.
    float wy = textureLod(uWaterLevel, uv, 0.0).r;
    if (wy < -9000.0) return 0.0;
    // 8 cm of blend, not 25: enough to keep the waterline from aliasing into a
    // hard stair, small enough that it is still the waterline.
    return 1.0 - smoothstep(wy + lift, wy + lift + 0.08, wp.y);
}

// 0..1 at a world position. Wet below the mark the water left, blending out over
// a short band above it — capillary rise in soil, bark and concrete is real, and
// a hard line here would only reintroduce the seam we removed at the waterline.
// DEBUG VIEW (r_wtr_wet negative). Every input to this function has been verified
// from the CPU side — the map is full, the UBO arrives, the binding is the shared
// one — and reeds and a log standing in that water are still dry. What has never
// been observed is the RETURN VALUE at those pixels, so that is what this shows.
bool shoreWetDebug() { return L.shorewet.w < 0.0; }

// ⭐ FILTER THE ANSWER, NOT THE HEIGHTS. The tile is 12.5 cm per texel and the
// map must be point-sampled — .r is a world height with a -10000 "dry" sentinel,
// and interpolating that against a real height invents waterlines (the four-metre
// dead band along every shore, earlier in this arc). Point sampling, though, puts
// the wet edge on screen in 12.5 cm squares, which up close is a staircase and at
// a distance is not — "rough and sharp from here, soft from there".
//
// Both wants are satisfiable at once: take the four texels by hand, run the height
// test SEPARATELY in each, and bilinear-blend the 0..1 results. No height is ever
// mixed with another, so the sentinel cannot leak; a tap with no record simply
// contributes dry, which is what the edge of a wetted patch should fade toward.
// Lace, not a wash. Foam left on sand is a torn film of bubbles, and a smooth
// gradient of it reads as a paint stain — the break-up is most of the effect.
float swHash(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float swNoise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(swHash(i), swHash(i + vec2(1, 0)), f.x),
               mix(swHash(i + vec2(0, 1)), swHash(i + vec2(1, 1)), f.x), f.y);
}

// .x = wetness 0..1, .y = FOAM 0..1 — both out of the same four taps.
//
// ⭐ The foam needs no channel of its own. `.g` decays from 1 on a known clock
// (r_wtr_wet_dry), so it already IS "how long ago did the water let go of this
// column" — and raising it to a power just rescales that clock: with
// g = 0.01^(t/dry), g^k dies in dry/k seconds. So the exponent (L.shorefoam.y,
// computed CPU-side as dry / foam_life) picks the last two or three seconds out
// of a twenty-five second memory for free.
vec2 shoreWetnessTile(vec3 wp)
{
    if (abs(L.shorewet.w) <= 0.001) return vec2(0.0);
    vec2 uv = (wp.xz - L.shorewet.xy) * L.shorewet.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return vec2(0.0);
    // Fade at the tile border, or the world gains a visible 32 m square of damp.
    vec2  e    = min(uv, vec2(1.0) - uv);
    float edge = smoothstep(0.0, 0.06, min(e.x, e.y));

    vec2  ts = vec2(textureSize(uShoreWet, 0));
    vec2  t  = uv * ts - 0.5;
    vec2  f  = fract(t);
    vec2  b  = (floor(t) + 0.5) / ts;
    vec2  acc = vec2(0.0);
    const float expo = max(L.shorefoam.y, 1.0);
    for (int i = 0; i < 4; ++i) {
        vec2  o = vec2(float(i & 1), float(i >> 1));
        vec2  s = textureLod(uShoreWet, b + o / ts, 0.0).rg;
        if (s.g <= 0.002) continue;                        // no record here = dry
        float w = mix(1.0 - f.x, f.x, o.x) * mix(1.0 - f.y, f.y, o.y);
        // 6 cm of blend above the mark. The mark is now the exact height the
        // water reached in this column, and the bilinear above carries the rest
        // of the smoothing — the old 25 cm smear threw away most of the precision
        // the contact test had just been given.
        float under = 1.0 - smoothstep(s.r, s.r + 0.06, wp.y);
        acc.x += w * s.g * under;
        // ⚠ THE STRANDLINE, not the whole wetted patch. Foam keyed to recency
        // alone came out in hard RECTANGLES: `g` is near-binary per texel (a column
        // either got touched or it did not, and the ground it is tested against is
        // itself a 15 cm staircase out of the height map), so a bilinear ramp across
        // ONE 12.5 cm texel is all the softening there is — and at a grazing angle
        // three metres away, 12.5 cm of ground is a slab of screen.
        //
        // Gating on HEIGHT BELOW THE MARK fixes it at the root: wp.y is per pixel
        // and continuous, so the band's shape stops coming from the texel grid
        // altogether. It is also where the foam actually is — a strandline sits at
        // the top of what the last surge reached, not over the whole wet flat.
        acc.y += w * pow(s.g, expo) * under * smoothstep(s.r - 0.30, s.r - 0.04, wp.y);
    }
    acc *= edge;
    acc.x *= abs(L.shorewet.w);
    return clamp(acc, 0.0, 1.0);
}

float shoreWetness(vec3 wp) { return shoreWetnessTile(wp).x; }

// The two together. The standing fact sets the floor — every shore on the level
// is wet from the first frame it is visible. The local record can only ADD: a
// receding waterline leaves a strip above the current level that then dries,
// and that part genuinely is about recent history, so it genuinely is local.
// RAW level-map content at this pixel: the stored water height, or -1e9 where the
// map says dry. r_wtr_wet -3 paints it directly — every previous round guessed at
// what the map contains from how the test BEHAVED, and guessed wrong three times.
float shoreWaterLevelRaw(vec3 wp)
{
    if (L.waterlvl.z <= 0.0) return -1e9;
    vec2 uv = (wp.xz - L.waterlvl.xy) * L.waterlvl.z;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return -1e9;
    float wy = textureLod(uWaterLevel, uv, 0.0).r;
    return (wy < -9000.0) ? -1e9 : wy;
}

float shoreWet(vec3 wp)
{
    // r_wtr_wet -3: SHOW THE MAP. blue = no water stored here; otherwise the
    // height DIFFERENCE to this fragment — green when the stored surface is below
    // the pixel (should read dry), red when above (should read wet), brightness
    // rising over the first two metres. A stripe of red where the ground is
    // plainly above the river then names its own cause.
    // r_wtr_wet -4: the TILE half alone — the contact record, with the level map
    // taken out of the picture. The two halves answer different questions now
    // (submerged / recently touched) and the combined view cannot say which one
    // painted a given pixel, which is the first thing every symptom here turns on.
    if (L.shorewet.w < -3.5) return clamp(shoreWetness(wp), 0.0, 1.0);
    if (L.shorewet.w < -2.5) {
        float wy = shoreWaterLevelRaw(wp);
        if (wy < -1e8) return 0.0;                 // painted black = "map says dry"
        return clamp((wy - wp.y) * 0.5 + 0.5, 0.0, 1.0);
    }
    float sta = shoreWetnessStatic(wp) * abs(L.shorewet.w);
    // r_wtr_wet -2: show the STATIC half ALONE. The combined view cannot tell
    // "the level map is working" from "the local tile is covering for it", and
    // that is exactly the question every remaining symptom turns on — a boundary
    // that moves with the camera can only come from the tile.
    if (L.shorewet.w < -1.5) return clamp(sta, 0.0, 1.0);
    float dyn = shoreWetness(wp);                       // recent contact, decays
    return clamp(max(dyn, sta), 0.0, 1.0);
}

// FOAM the backwash left behind, 0..1. Two gates beyond the recency above:
//   ABOVE THE STILL LINE — below it the water layer is still drawn over the
//   ground, so foam there is either invisible or a second, wrong scum line. The
//   band that matters is exactly the strip the surge climbed and let go of.
//   TORN — multiplied by two octaves of drifting noise, or it is a paint stain.
float shoreFoam(vec3 wp)
{
    if (L.shorefoam.x <= 0.001 || L.shorewet.w < 0.0) return 0.0;
    float f = shoreWetnessTile(wp).y;
    if (f <= 0.004) return 0.0;
    float wy = shoreWaterLevelRaw(wp);
    if (wy > -1e8) f *= smoothstep(wy - 0.02, wy + 0.06, wp.y);
    float n = swNoise(wp.xz * 2.6 + vec2(L.shorefoam.z * 0.07, 0.0)) * 0.62
            + swNoise(wp.xz * 8.3 - vec2(0.0, L.shorefoam.z * 0.11)) * 0.38;
    return clamp(f * smoothstep(0.30, 0.85, n) * L.shorefoam.x, 0.0, 1.0);
}

#endif // SHORE_WET_GLSL
