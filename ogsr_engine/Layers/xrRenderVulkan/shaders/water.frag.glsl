#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — WATER surface, fragment stage.
//
// A blended surface layer, NOT an opaque one. The bottom is already in the
// colour target when this runs (water is drawn after the whole opaque world +
// sky, writes no depth, and is excluded from the depth prepass), so the "see
// through the water" half of the picture costs nothing: it is the destination
// side of the blend. That is why there is no scene-colour copy here.
//
// Composite, with PREMULTIPLIED alpha (src = ONE, dst = ONE_MINUS_SRC_ALPHA):
//
//   out.rgb = F·reflection + (1-F)·(1-T)·scatter + sun glitter
//   out.a   = 1 - T·(1-F)
//
// where F is the Fresnel reflectance of the surface and T = exp(-k·thickness)
// the Beer-Lambert transmittance of the water column the view ray crosses. The
// three energy terms sum to one: F reflected, (1-F)(1-T) scattered back out of
// the body, (1-F)T transmitted from the bottom — which is exactly what the
// blend leaves of the destination.
//
// Waves are an ANALYTIC slope field (no normal map): stock content ships no
// water normal texture, and an analytic sum gives exact derivatives, which is
// what the sun glitter needs in order to spread into a path instead of
// sparkling into per-pixel noise.
//
// WATER_LEAN — the Switch twin (water_lean.frag.spv, picked by r_wtr_lean). Same
// surface model, minus everything that needs a pass of its own or a pile of taps:
// no spectral field, no ripple sim, no local fetch map, no shelter query (13
// rain-map taps), no refraction copy / caustics, no shore break or grid hand-over.
// Six swell octaves instead of nine, and the sun shadow comes from the R1
// receivers' one-gather-per-cascade lookup at the SURFACE (the half-res sun mask
// would describe the bottom: water is not in the prepass).
#ifdef WATER_LEAN
#define WATER_NO_FFT
#define WATER_NO_RIPPLE
#define WATER_SWELL_OCT 6
#endif
#include "light_ubo.glsl"       // set 1: Lighting UBO + sky cubes + shadow maps
#include "wet_common.glsl"      // rainRipples (drop-impact rings — reused verbatim)
#include "vsm_sample.glsl"      // vsmSunShadow (screen-space VSM mask)
#include "shadow_common.glsl"   // sunShadow / cascTap (cascade fallback)
#include "env_common.glsl"      // rainVis + skyAmbient (via sky_ambient.glsl)
#ifdef WATER_LEAN
#include "nx_sun_shadow.glsl"   // nxSunShadowCov: cascade 0 / 1 / far, one gather each
#endif

// Scene depth of whatever is BEHIND the water — the bottom. The pass binds NO
// depth attachment at all (same shape as the light-cone pass): the depth image
// is a plain sampled texture here, and the LEQUAL test is done by hand below.
// That sidesteps the "sample the attachment you are testing against" feedback
// loop entirely; water writes no depth, so the hardware test bought nothing but
// early-Z on a few big quads.
layout(set = 0, binding = 0) uniform sampler2D uSceneDepth;

// Half-resolution copy of the HDR scene taken just before this pass — the
// bottom, as a texture we can sample at a bent UV. Half res on purpose: murky
// water refracts through centimetres of moving surface, so the extra sharpness
// would be thrown away by the very distortion it is sampled through, and the
// full-res copy costs 4x the bandwidth and VRAM for it.
layout(set = 0, binding = 3) uniform sampler2D uRefract;

// Pool mask of the ripple sim (surface height per texel, very negative = dry).
// Read ONLY by r_wtr_debug 7: "does the sim think this puddle is its own body of
// water?" is not answerable by staring at ripples, and guessing it cost two
// wrong fixes.
layout(set = 0, binding = 4) uniform sampler2D uPoolMask;

// The push block is shared with the vertex/TESE stages. This stage ignores the
// matrix but needs `basin`: the UBO is per-frame and therefore per-WORLD, and
// the whole point of the fetch work is that a flooded cellar and an open pond
// are not the same body of water. Per-surface data has nowhere else to go.
layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 basin;   // x = span of this water body in metres (0 = open water)
} pc;
//
// The wave field, the tuning UBO and the ripple-sim sampler all live in
// water_common.glsl — SHARED with the tessellation evaluation stage, which
// displaces geometry by the very same function this stage shades with. Two
// copies of a wave function is two waves.
#include "water_common.glsl"
#include "water_shore.glsl"   // the break: phased by DEPTH, so it arrives at the beach

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main()
{
    // ---- manual depth test (the pass binds no depth attachment) -----------
    // D3D-style [0,1] depth, larger = farther: this fragment is hidden whenever
    // the opaque scene in front of it is NEARER. The epsilon keeps water that is
    // exactly coplanar with its own bottom (a puddle modelled ON the ground)
    // from stippling itself away.
    vec2  suv0 = gl_FragCoord.xy * L.ao_params.xy;
    float zScn = textureLod(uSceneDepth, suv0, 0.0).r;
    if (gl_FragCoord.z > zScn + 1e-6) discard;

    // ---- geometry ---------------------------------------------------------
    vec3  toEye = L.eye_pos.xyz - vWorldPos;
    float dist  = length(toEye);
    vec3  V     = toEye / max(dist, 1e-4);
    vec3  Ng    = normalize(vNormal);
    if (dot(Ng, V) < 0.0) Ng = -Ng;      // X-Ray winding is uncorrelated with normals

    // ---- waves ------------------------------------------------------------
    // ONE field for geometry and shading: waterField is what the TESE displaced
    // this vertex by, evaluated here per pixel. Swell (wind-driven, domain-
    // warped octaves) + the interactive ripple sim + capillary micro-ripple.
    // Two things the wave field used to know nothing about, and between them the
    // whole "why is the flooded cellar simulating surf" complaint:
    //   FETCH   — the span of this pool. A 28-metre swell cannot exist in a room
    //             five metres across; only the centimetre ripple can.
    //   SHELTER — whether there is sky overhead at all. Wind waves need wind.
    // ⚠ pc.basin.x is the span of the MESH, and level water is one mesh: the same
    // number reached the river and the two-metre circle inside a well in the same
    // visual. waterLocalFetch measures this texel's own body out of the pool mask
    // and can only make it smaller — open water keeps pc.basin.x untouched.
#ifdef WATER_LEAN
    float fetch   = pc.basin.x;    // no sim tile, so no local fetch map: the mesh's own span
    float shelter = 1.0;
#else
    float fetch   = waterLocalFetch(vWorldPos.xz, pc.basin.x);
    float shelter = waterShelter(uRainMap, vWorldPos);
#endif
    // What is left of the LONG swell here — the waves that run up a shore, bare
    // the bottom and break into foam. Drives the swash and the foam band below.
    float energy  = waterSwellEnergy(fetch) * shelter;
    vec4  field   = waterField(vWorldPos.xz, dist, fetch, shelter);
    vec2  dh      = field.yz;
    float lod   = clamp(1.0 - dist / max(W.p3.x, 1.0), 0.0, 1.0) * 0.85 + 0.15;

    // Rain rings — the same Lagarde-style drop layers the puddles use, so a
    // downpour stipples ponds and puddles with ONE look. rainVis gates by the
    // rain occlusion map: water under a roof stays smooth.
    float rain = W.p1.w;
    if (rain > 0.01) {
        float vis = rainVis(vWorldPos);
        dh += rainRipples(vWorldPos.xz, W.p0.x) * (rain * vis * 0.9 * lod);
    }

    // Slope -> normal. Water bodies are horizontal, so the slope field is built
    // in world XZ; `abs(Ng.y)` blends back to the geometric normal on anything
    // tilted (a sloped stream surface keeps its own orientation).
    vec3 Nw = normalize(vec3(-dh.x, 1.0, -dh.y));
    vec3 N  = normalize(mix(Ng, Nw, clamp(abs(Ng.y), 0.0, 1.0)));
    if (dot(N, V) < 0.0) N = Ng;          // a steep wave must never face away

    // ---- water column the view ray crosses --------------------------------
    // zview = p43 / (zndc - p33) — the same reconstruction the SSR puddles and
    // the light cones use (p33 = L.cam_dir.w, p43 = L.cam_rightT.w).
    vec2  suv  = suv0;
    float zndc = zScn;
    float zvS  = L.cam_rightT.w / (zndc          - L.cam_dir.w);
    float zvW  = L.cam_rightT.w / (gl_FragCoord.z - L.cam_dir.w);
    // View-space Z is the depth ALONG THE CAMERA AXIS; the ray basis converts
    // it to distance along this pixel's ray, which is the real path length.
    vec2  ndc  = vec2(suv.x * 2.0 - 1.0, 1.0 - 2.0 * suv.y);
    float rl   = length(L.cam_dir.xyz + L.cam_rightT.xyz * ndc.x + L.cam_topT.xyz * ndc.y);
    float thick = max(zvS - zvW, 0.0) * rl;
    // The bottom can be MISSING (sky behind the water at a shoreline gap, or a
    // hole in the level): zndc == 1 gives a huge thickness, which is the right
    // answer anyway — treat it as deep.
    thick = min(thick, 200.0);
    // ...and the VERTICAL drop from the surface to the bottom, which is a
    // different number and the one the shoreline actually needs. `thick` is a
    // PATH LENGTH: at a grazing angle the ray crosses metres of water through a
    // sheet a centimetre deep, so a fade keyed to it reads "deep" everywhere and
    // the water/land seam stays a razor cut no matter how r_wtr_shore is set.
    // The path vector is rayDir * thick, so its vertical component is thick*|V.y|
    // — exact at any angle, and it keeps the units honest for the swash below,
    // which is a wave HEIGHT in metres and was being added to a path length.
    float depthV = thick * abs(V.y);

    // ---- sun ---------------------------------------------------------------
#ifdef WATER_LEAN
    float sunCov;
    float sunSh = nxSunShadowCov(vWorldPos, sunCov);   // 1.0 past the far map
#else
    float sunSh = (L.shadow_params.w > 0.5) ? vsmSunShadow(suv) : sunShadow(vWorldPos);
#endif
    vec3  Ld    = normalize(-L.sun_dir.xyz);

    // ---- reflection --------------------------------------------------------
    // Schlick with water's F0 = 0.02: nearly transparent looking down, a mirror
    // at grazing angles. This one term is most of what reads as "water".
    float NoV = clamp(dot(N, V), 0.0, 1.0);
    float F   = 0.02 + 0.98 * pow(1.0 - NoV, 5.0);

    // Roughness grows with distance: the sub-pixel chop that the LOD fade just
    // removed from the NORMAL has to come back as spread in the SPECULAR, or the
    // sun path turns into a field of flickering white dots.
    float rough = clamp(W.p3.y + dist * 0.0016, 0.02, 0.6);

    vec3 R = reflect(-V, N);
    R.y = abs(R.y);                       // never sample below the horizon (no SSR yet)
    vec3 refl;
    if (L.ibl_params.x > 0.004) {
        refl = textureLod(uSkySpec, R, rough * L.ibl_params.z).rgb;   // prefiltered sky probe
    } else {
        // NO PROBE (r_ibl 0). The diffuse fallback is an SH irradiance: correct
        // as fill light, but so low-frequency that the mirror it produces is
        // almost a constant — and a reflection with no gradient cannot show a
        // wave, however big the wave is. ("Абсолютный штиль" with r_ibl 0 is
        // this, not the wave amplitude.) Synthesize the one gradient that always
        // exists outdoors instead: haze at the horizon, sky overhead. The ripple
        // then rides that ramp and becomes visible.
        vec3 zenith  = skyAmbient(vec3(0.0, 1.0, 0.0)) * 1.35;
        vec3 horizon = mix(L.fog_color.rgb, skyAmbient(normalize(vec3(R.x, 0.15, R.z))), 0.5);
        refl = mix(horizon, zenith, smoothstep(0.0, 0.45, R.y));
    }
    refl *= W.p2.w;
    // Water under a roof cannot mirror a sky it cannot see. Both branches above
    // hand back SKY — the probe samples it, the fallback synthesizes it — and at
    // a grazing angle Fresnel goes to 1, so that sky covers the ENTIRE surface.
    // In a flooded cellar that is a bright white film over the water with no
    // source in the room, which is exactly what "покрыта пенкой" describes: it
    // is not foam at all, it is a reflection of outdoors indoors. Not driven to
    // zero — an interior still has walls and lamps to reflect, and water that
    // reflects nothing is the black water this whole pass was written to fix.
    refl *= mix(0.22, 1.0, shelter);

    // Sun glitter — GGX with the wave normal. Deliberately NOT env_common's
    // sunSpec(): that one early-outs when the IBL probe is off, and the glitter
    // is the single most alive thing on a water surface.
    vec3  H   = normalize(V + Ld);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);
    float a   = max(rough * rough, 0.004);
    float dd  = NoH * NoH * (a * a - 1.0) + 1.0;
    float D   = (a * a) / (3.14159265 * dd * dd);
    float Fs  = 0.02 + 0.98 * pow(1.0 - VoH, 5.0);
    vec3  glint = vec3(D * 0.25 * Fs) * L.sun_color.rgb * sunSh * W.p3.z;

    // ---- body --------------------------------------------------------------
    // STILL WATER IS MURKY WATER. A pond is flushed by rain, inflow and wind
    // mixing; a flooded cellar has none of that, so silt, rust and algae stay in
    // suspension and the bottom goes dim after a few centimetres. Same shelter
    // signal, because the thing it really measures is "is this water connected to
    // the weather" — which is exactly what decides whether it stays clear.
    float kMurk = W.p1.z * mix(W.p10.z, 1.0, shelter);
    // Beer-Lambert through the column. Shallow water at the shoreline keeps the
    // bottom visible and fades into the murk as it deepens — the gradient that
    // makes a pond read as having a shape instead of being a painted plane.
    float T = exp(-kMurk * thick);
    // Light that gets INTO the body: sky from above plus the sun it still sees.
    // Sky light entering the body is gated the same way and for the same reason:
    // no sky over the room means no sky pouring into the water in it. The sun
    // term needs no gate — the shadow map already zeroes it indoors.
    vec3  bodyLight = skyAmbient(vec3(0.0, 1.0, 0.0)) * 0.75 * mix(0.3, 1.0, shelter)
                    + L.sun_color.rgb * sunSh * max(Ld.y, 0.0) * 0.55;
    vec3  scatter   = W.p2.rgb * bodyLight;

    // ---- the bottom, seen THROUGH the surface ------------------------------
    // Up to here the bottom arrived for free as the destination of the blend —
    // correct, but a blend cannot MOVE what is behind it, so the riverbed sat
    // there dead straight while the surface rippled over it. Sampling the scene
    // copy ourselves buys three things at once: the refraction offset, a
    // PER-CHANNEL Beer-Lambert tint (green survives, red dies first — the thing
    // that actually reads as "deep"), and somewhere to put the caustics.
    vec3  bottom     = vec3(0.0);
    float haveBottom = 0.0;
#ifndef WATER_LEAN
    if (W.p9.w > 0.0) {
        // Offset along the surface slope, shrinking with distance so the far
        // field does not smear, and with thickness so a puddle barely bends.
        vec2 off = -dh * W.p9.x * clamp(thick, 0.0, 1.5) / (1.0 + dist * 0.05);
        vec2 ruv = clamp(suv + off, vec2(0.001), vec2(0.999));
        // A refraction offset must never reach IN FRONT of the water: sampling
        // a foreground object drags its colour into the pond (the classic
        // "the rock is smeared under the surface" artefact). If the offset
        // landed nearer than this fragment, fall back to the straight sample.
        float zOff = textureLod(uSceneDepth, ruv, 0.0).r;
        if (zOff < gl_FragCoord.z) ruv = suv;
        bottom = textureLod(uRefract, ruv, 0.0).rgb;
        haveBottom = 1.0;

        // Caustics ride ON the bottom, so they are attenuated by the same water
        // column the bottom is — light that has to come back up through the murk.
        float caus = waterCaustic(vWorldPos.xz, thick, L.sun_dir.xyz, dist, fetch, shelter)
                   * sunSh * clamp(-L.sun_dir.y, 0.0, 1.0);
        bottom += bottom * caus * L.sun_color.rgb;
    }
#endif
    // Per-channel transmittance: the murk colour IS the absorption spectrum.
    vec3 Tc = exp(-kMurk * thick * (vec3(1.0) - clamp(W.p2.rgb * 3.0, 0.0, 0.95)));

    vec3  rgb   = F * refl + (1.0 - F) * (1.0 - T) * scatter + glint;
    float alpha = clamp(1.0 - T * (1.0 - F), 0.0, 1.0);
#ifndef WATER_LEAN
    if (haveBottom > 0.0) {
        // We own the pixel now: add the transmitted bottom ourselves and let
        // alpha go to 1, instead of leaving the destination to shine through
        // untinted and unbent. The shore fade below still returns the pixel to
        // the destination where the layer vanishes.
        rgb  += (1.0 - F) * Tc * bottom;
        alpha = 1.0;
    }
#endif

    // ---- shoreline ---------------------------------------------------------
    // Absorption alone does NOT soften the water/terrain seam, because the
    // Fresnel term puts a floor under the alpha: at a grazing angle F -> 1, so
    // even a millimetre-deep film stayed a full mirror and the intersection with
    // the ground came out as a hard, aliased cut. Physically a film that thin
    // reflects almost nothing coherently — so fade the WHOLE layer out over the
    // last few centimetres of depth, which is what makes the edge dissolve.
    // SWASH: the waterline has to MOVE. A wave arriving at a beach runs up it
    // and drains back; a shoreline that only fades by depth is a painted line
    // whatever else the surface does. The column thickness used for the fade is
    // therefore offset by the local wave height — crest arrives, the water
    // climbs the sand; trough follows, the sand is bared. Free, because the
    // wave height is already in hand.
    // Swash is a LONG-wave phenomenon — a run-up needs a wave train with metres
    // between crests. In a small pool there is none, so the waterline must stand
    // still even though the capillary ripple is still alive on it.
    // field.w, not field.x: the SWELL alone runs the waterline up and down. See
    // waterField — the ripple in field.x is a local disturbance, and letting it
    // drive the run-up made your own footstep rings erase the water layer.
    // ⚠ NOT `field.w * gain`, and no longer a function of the WIND either. The
    // swell is symmetric in time and has no front, so scaling it only made the
    // whole waterline breathe in place; and a wind-phased run-up slides ALONG a
    // beach instead of arriving at it. The break is phased by the still water
    // DEPTH (water_shore.glsl), which is the one field that already knows where
    // the shore is — iso-depth lines are the shoreline.
    //
    // Falls back to the wind-phased swash outside the tile, where there is no
    // depth map to ask; at that distance the difference is a few pixels.
    vec2  sw    = vec2(0.0);
    float shoreW = 0.0;
#ifdef WATER_LEAN
    float dShore = -1e9;            // no depth field without the sim tile
#else
    float dShore = (W.p11.w > 0.0) ? waterShoreDepth(vWorldPos.xz) : -1e9;
#endif
    if (dShore > -1e8) {
        sw     = waterShoreBreak(dShore, W.p0.x, W.p11.z, W.p10.x, W.p10.w, W.p11.x);
        shoreW = sw.y;
    } else {
        sw = waterSwash(vWorldPos.xz, W.p0.x, W.p10.x, W.p10.w, W.p1.xy, energy);
    }
    float swash  = sw.x;
    // ⚠⚠ THE RUN-UP IS A FILM, NOT A COLUMN — and on the grid it is not a fake at
    // all. `swash` is how high the sheet CLIMBS; adding it to the depth silently
    // claims that much water is STANDING there, so at 45 cm of run-up the bank came
    // out wearing a 45 cm slab of opaque water with a vertical face on it.
    //
    // The grid's vertex stage now lays the sheet ON the sand, so the depth buffer
    // already reports the true couple of centimetres and nothing needs faking. The
    // level's own far-field polygons are flat and cannot do that, so they keep the
    // offset — capped at a film, because a swash tongue is something you see the
    // sand through and what makes it read is the FOAM riding on it, not its body.
    float thickS;
    if (pc.basin.y > 0.5) {
        thickS = max(depthV, 0.0);                   // grid: the geometry IS the run-up
    } else {
        thickS = max(depthV + swash, 0.0);
        if (W.p11.w > 0.0) thickS = min(thickS, max(depthV, 0.0) + W.p11.w);
    }
    float shore  = smoothstep(0.0, max(W.p4.x, 0.001), thickS);
    rgb   *= shore;
    alpha *= shore;

    // Foam along that same band. A shoreline reads as a shore because of the
    // scum line, not because the water stops; a thin animated band hides what
    // is left of the seam and costs two noise taps. Broken up with the wave
    // field so it is a ragged edge rather than a contour line.
    // The band can only be as wide as the wave is actually able to bare the
    // bottom. Keyed to the absolute depth alone it said "foam wherever the water
    // is shallower than 45 cm" — so a flooded cellar, which is shallower than
    // that everywhere, came out covered in shoreline foam end to end with no
    // shoreline in sight. Scaled so an open pond at the default wave height
    // still gets the full r_wtr_foam_w and nothing about open water changes.
    float waveAmp = W.p0.y * energy;                 // metres of swell really here
    // The band has to be able to cover the RUN-UP as well, or the surge climbs the
    // sand as a pane of clear water with the scum line left behind at the still
    // level — which is the one thing that would give the whole effect away.
    float foamW   = min(max(W.p4.z, 0.01), (waveAmp + swash) * 2.5);
    if (W.p4.y > 0.001 && foamW > 0.005) {
        float band = 1.0 - smoothstep(0.0, foamW, thickS);
        band *= shore;                                   // dies with the layer itself
        float n = vNoise(vWorldPos.xz * 3.1 + vec2(W.p0.x * 0.05, 0.0)) * 0.6
                + vNoise(vWorldPos.xz * 9.7 - vec2(0.0, W.p0.x * 0.09)) * 0.4;
        // A BORE IS WHITE, a backwash is a lace of bubbles. sw.y carries that
        // difference straight from the swash cycle, so the front of the surge comes
        // up bright and what drains back off it does not.
        float f = clamp(band * (0.45 + 0.85 * n) * (1.0 + 1.6 * sw.y), 0.0, 1.0) * W.p4.y;
        // Foam is lit by the same sky/sun as the body, never pure white.
        rgb   += f * bodyLight * 0.85;
        alpha  = clamp(alpha + f, 0.0, 1.0);
    }

    // ---- WHITEWATER --------------------------------------------------------
    // What a broken wave actually IS. Not a band along a contour and not a tint:
    // an OPAQUE, matte, aerated sheet that is made at the front of the bore and
    // left behind it, thinning as the water drains back off the sand. This is the
    // "foams when it hits the ground" half — it appears exactly where the crest
    // can no longer stand over the depth beneath it, which the break model above
    // has already worked out.
    if (shoreW > 0.002 && W.p11.y > 0.001) {
        float wn = vNoise(vWorldPos.xz * 2.3 + vec2(W.p0.x * 0.30, W.p0.x * -0.21)) * 0.60
                 + vNoise(vWorldPos.xz * 7.4 - vec2(W.p0.x * 0.44, 0.0)) * 0.40;
        // Torn at the edges, solid in the middle of the tongue — a uniform wash
        // reads as fog on the water.
        float wf = clamp(shoreW * smoothstep(0.22, 0.78, wn) * W.p11.y, 0.0, 1.0);
        // Gated on the FILM, not on `shore`. The body of a swash tongue is a
        // couple of centimetres and therefore nearly transparent — but the foam on
        // it is not, and dimming the foam by the body's own alpha would leave the
        // run-up as a faint smear. Foam appears wherever there is any water at all.
        wf *= smoothstep(0.0, 0.025, thickS);
        // Aerated water is diffuse and BRIGHT: it hides the bottom (alpha) instead
        // of reflecting the sky, so it goes in as light plus opacity, not as gloss.
        rgb   = mix(rgb, bodyLight * 1.15, wf);
        alpha = clamp(alpha + wf * (1.0 - alpha), 0.0, 1.0);
    }

    // ---- WHITECAPS ---------------------------------------------------------
    // Foam ON THE WAVE, not only where it meets the land. This is the cue that
    // separates water from a polished floor at ANY distance, and the one this
    // surface never had: crests tear and go white while the troughs stay dark, so
    // the wave field becomes readable even out where the mesh is far too coarse to
    // displace anything. It is also the honest way round — a shore foam line on a
    // mirror reads as a decal; a broken crest reads as water.
    //
    // Two gates, because both are physical: near the TOP of the crest, and STEEP.
    // A lazy swell does not break however high it is, which is what keeps a calm
    // pond from coming out speckled.
    if (W.p4.y > 0.001 && waveAmp > 0.02) {
        float capH   = clamp(field.w / max(waveAmp, 0.005), 0.0, 1.0);
        float steep  = length(field.yz);
        float caps   = smoothstep(0.52, 0.90, capH)
                     * smoothstep(0.18, 0.70, steep)
                     * smoothstep(0.03, 0.12, waveAmp);
        if (caps > 0.002) {
            // Torn, and DRIFTING with the wind — foam that sits still on a moving
            // wave is the giveaway that it was painted on.
            vec2 drift = W.p1.xy * (W.p0.x * 0.35);
            float cn = vNoise(vWorldPos.xz * 1.9 - drift * 1.9) * 0.62
                     + vNoise(vWorldPos.xz * 6.1 - drift * 3.4) * 0.38;
            float cf = clamp(caps * smoothstep(0.32, 0.82, cn), 0.0, 1.0) * W.p4.y;
            rgb   += cf * bodyLight * 0.95;
            alpha  = clamp(alpha + cf * 0.85, 0.0, 1.0);
        }
    }

    // ---- SPECTRAL WHITEWATER (the Jacobian) --------------------------------
    // The same idea one level deeper. The heuristic above asks "is this vertex
    // high, and is it steep" — a decent proxy, and all the analytic field can
    // offer. The spectral field can answer the actual question: has the surface
    // FOLDED OVER ITSELF here? The horizontal displacement is known, so the
    // determinant of its Jacobian says so outright. Below one, water is being
    // squeezed into less area than it came from; past zero it has turned inside
    // out, which is what a breaking wave IS. It also lands on the crest FRONT,
    // where waves really break, instead of symmetrically around the peak.
#ifndef WATER_LEAN
    if (W.p13.x > 0.5 && W.p13.y > 0.0) {
        float jf = waterFFTFoam(vWorldPos.xz, fetch, shelter);
        if (jf > 0.002) {
            // Torn and drifting, for the same reason the whitecaps above are:
            // foam that holds still on a moving wave reads as painted on.
            vec2  drift = W.p1.xy * (W.p0.x * 0.35);
            float cn = vNoise(vWorldPos.xz * 2.7 - drift * 2.2) * 0.60
                     + vNoise(vWorldPos.xz * 8.3 - drift * 4.1) * 0.40;
            float cf = clamp(jf * smoothstep(0.22, 0.85, cn), 0.0, 1.0);
            rgb   += cf * bodyLight * 0.95;
            alpha  = clamp(alpha + cf * 0.85, 0.0, 1.0);
        }
    }
#endif

    // ---- fog ---------------------------------------------------------------
    // The bottom behind this pixel already carries its own fog, so haze is
    // applied by FADING THE LAYER OUT (premultiplied: scale rgb and alpha
    // together) — distant water dissolves into the same haze as everything else
    // instead of painting a second fog on top of an already-fogged background.
    float fog = clamp(dist * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
    rgb   *= (1.0 - fog);
    alpha *= (1.0 - fog);

    // ---- near-field grid cross-fade ---------------------------------------
    // Two meshes now cover the same water: the level's own polygons (hundreds of
    // metres per triangle, flat) and the dense camera-locked grid that actually
    // has waves in it. Both are PREMULTIPLIED layers, so drawing both would make
    // the near water twice as opaque — they have to hand over, not overlap.
    // Complementary weights out of one function, so they sum to exactly 1 at every
    // pixel and the seam cannot show as either a hole or a double.
#ifndef WATER_LEAN
    if (W.p6.w > 0.0 && (pc.basin.y > 0.5 || pc.basin.w > 0.5)) {
        vec2  g = (vWorldPos.xz - W.p6.yz) / max(W.p6.w, 0.001);
        float w = 0.0;
        if (g.x > 0.0 && g.x < 1.0 && g.y > 0.0 && g.y < 1.0) {
            vec2 e = min(g, vec2(1.0) - g);
            w = smoothstep(0.0, 0.12, min(e.x, e.y));
        }
        // pc.basin.y = "I am the grid"; pc.basin.w = "the grid is being drawn
        // this frame" (set on the LEVEL polygons, which must get out of its way).
        float k = (pc.basin.y > 0.5) ? w : (1.0 - w);
        rgb   *= k;
        alpha *= k;
    }
#endif

    // ---- debug (r_wtr_debug) ----------------------------------------------
    int dbg = int(W.p3.w + 0.5);
    if (dbg > 0) {
        vec3 c = vec3(0.0);
        if      (dbg == 1) c = vec3(1.0, 0.0, 1.0);                    // flat magenta: does the pass run?
        else if (dbg == 2) c = vec3(clamp(depthV / 3.0, 0.0, 1.0));    // VERTICAL depth (white = 3 m+)
        else if (dbg == 3) c = N * 0.5 + 0.5;                          // wave normal
        else if (dbg == 4) c = vec3(alpha);                            // composite alpha
        else if (dbg == 5) c = vec3(F);                                // Fresnel
        // Fetch: green = open water (full swell), red = a pool too small to
        // carry one. Answers "why is this puddle flat / why is that cellar
        // heaving" without a second guess about which surface it belongs to.
        else if (dbg == 6) c = vec3(1.0 - energy, energy, shelter);   // blue = open to the sky
#ifndef WATER_LEAN
        // 7 = the ripple sim's POOL MASK. Red = this texel is DRY to the sim (no
        // wave may exist here), green/blue = wet, shaded by surface height so two
        // pools at different levels read as different colours. Outside the sim
        // tile: black.
        // 8 = the ripple FIELD itself: red = crest, blue = trough, black = still.
        // Answers the question the mask view cannot — whether a wave actually
        // EXISTS in the far puddles, or whether what reads as motion there comes
        // from somewhere else entirely.
        else if (dbg == 8) {
            float hr = waterRipple(vWorldPos.xz).x;
            c = vec3(clamp(hr * 60.0, 0.0, 1.0), 0.0, clamp(-hr * 60.0, 0.0, 1.0));
        }
        // 9 = the SPECTRAL field. Red = crest, blue = trough, green = the fold
        // (Jacobian) that whitewater comes from. Black means the field is not
        // reaching this water at all — which separates "the transform is dead"
        // from "the fetch gate has decided this pool is too small for it", and
        // those two have nothing in common but the symptom.
        else if (dbg == 9) {
            vec2  fsl; float fmx;
            vec4  ff = waterFFTAll(vWorldPos.xz, fetch, shelter, fsl, fmx);
            c = vec3(clamp(ff.y * 2.0, 0.0, 1.0), clamp(ff.w, 0.0, 1.0), clamp(-ff.y * 2.0, 0.0, 1.0));
        }
        else if (dbg == 7) {
            vec2 muv = (vWorldPos.xz - W.p6.yz) / max(W.p6.w, 0.001);
            if (muv.x < 0.0 || muv.x > 1.0 || muv.y < 0.0 || muv.y > 1.0) c = vec3(0.0);
            else {
                float ym = textureLod(uPoolMask, muv, 0.0).r;
                c = (ym < -9000.0) ? vec3(1.0, 0.0, 0.0)
                                   : vec3(0.0, 0.55 + 0.45 * sin(ym * 3.0), 0.6);
            }
        }
#endif
        outColor = vec4(c, 1.0);
        return;
    }

    outColor = vec4(rgb, alpha);
}
