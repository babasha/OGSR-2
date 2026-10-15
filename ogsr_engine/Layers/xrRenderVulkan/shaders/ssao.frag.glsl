#version 450
// xrRenderVulkan — GTAO (port of R4's gtao.h, "GTAO shader by Doenitz").
//
// Runs at HALF resolution between the depth prepass and the world color pass,
// so the only input is the scene depth (statics + alpha-tested statics).
// R4 reads view-space position+normal from the gbuffer; we reconstruct both
// from depth: position via the frustum-ray basis (camDir + right·tanX·ndc.x +
// top·tanY·ndc.y, zview = _43/(zndc−_33) — same scheme as light_cone.frag),
// normal via 4-neighbour position differences (pick the neighbour closer in
// depth on each axis so geometry edges don't smear the normal).
//
// The GTAO math itself is space-agnostic (any orthonormal frame): we feed it
// camera-RELATIVE world positions; the original's view-space screen axes map
// to the camera right/top unit vectors.
//
// Output: R8 visibility (1 = open, 0 = fully occluded). Pairs with
// ssao_blur.frag (depth-aware 3×3) before receivers sample it.

// MRT: target 0 = AO + bent normal (unchanged); target 1 = SSIL indirect light,
// gathered in the SAME horizon march (the occluder that raises the horizon is the
// surface that bounces light at us) from the PREVIOUS frame's lit colour — there
// is no lit colour in the prepass, and prev-frame is the temporal foundation.
layout(location = 0) out vec4 outAO;   // r = AO visibility (target is R16F — gba dropped with the retired bent normal)
layout(location = 1) out vec4 outIL;   // rgb = indirect radiance (HDR, pre-exposure), a = 1

layout(set = 0, binding = 0) uniform sampler2D uDepth;     // full-res scene depth (D32, prepass)
layout(set = 0, binding = 2) uniform sampler2D uNormal;    // normal G-buffer (rgb=worldN*0.5+0.5); a = SOURCE tag: 1 skinned, 0.5 static, 0 none
layout(set = 0, binding = 3) uniform sampler2D uPrevColor; // PREVIOUS frame's lit scene, half-res (linear HDR) — IL source

layout(push_constant) uniform PC {
    vec4 camDir;     // xyz = camera forward (unit); w = r_ssao_bias = grazing fade N·V threshold (fade AO→open below it → kills flat-floor banding)
    vec4 camRightT;  // xyz = right * tan(fovX/2), w = tan(fovX/2)
    vec4 camTopT;    // xyz = top   * tan(fovY/2), w = tan(fovY/2)
    vec4 zp;         // x = proj _33, y = proj _43, z = world radius (m), w = samples/side
    vec4 res;        // xy = AO target size, zw = 1 / AO target size
    vec4 dbg;        // x = r_ssao_debug mode (2 = depth, 3 = normal); y = SSIL on; z = SSIL firefly clamp; w = SSIL strength
    vec4 temporal;   // x = EMA α (0 = temporal off); y = per-frame jitter phase [0,1); z = MV valid; w = history valid (blur only)
} pc;

const float PI = 3.14159265;
const int   SLICES = 4;

// Was the Drobot fast-acos approximation (R4). Its ~0.04° error is SYSTEMATIC in
// the input angle, and on a flat surface viewed at a grazing angle (floor, distant
// slopes) the AO should be a constant 1.0 — so that smooth angle-dependent error
// becomes smooth distance-dependent BANDS that no amount of sampling/blur/temporal
// removes (the error is in the angle function, not the sample noise). Use the exact
// acos — at half-res the cost is negligible. Kept the name to avoid touching callers.
float fast_acos(float v)
{
    return acos(clamp(v, -1.0, 1.0));
}

// Camera-relative world position + view depth from the scene depth at uv.
// The reconstruction ray is built at the CENTER of the full-res depth texel the
// NEAREST sampler picks — not at the query uv. The prepass rasterized that depth
// at the texel center, so center-ray × depth lands exactly ON the surface; a
// query-uv ray would slide the point off it by the sub-texel mismatch, a snap-
// phase staircase on grazing ground that ends up as STRIPES in the reconstructed
// normal → in the AO of partially occluded areas (the fully-open integral is
// horizon-saturated and hides N noise, which is why open floors stayed clean).
vec4 fetchPos(vec2 uv)
{
    vec2 fs = vec2(textureSize(uDepth, 0));
    vec2 tc = (clamp(floor(uv * fs), vec2(0.0), fs - 1.0) + 0.5) / fs;   // CLAMP_TO_EDGE-consistent
    float zndc  = texture(uDepth, tc).r;
    float zview = clamp(pc.zp.y / (zndc - pc.zp.x), 0.0, 10000.0);
    vec2  ndc   = vec2(tc.x * 2.0 - 1.0, 1.0 - 2.0 * tc.y);   // D3D ndc (y up)
    vec3  ray   = pc.camDir.xyz + pc.camRightT.xyz * ndc.x + pc.camTopT.xyz * ndc.y;
    return vec4(ray * zview, zview);
}

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.res.zw;

    float zndc = texture(uDepth, uv).r;

    // r_ssao_debug 2: dump what the pass actually READS — linear view depth on
    // a 100 m ramp (black = near, white = far/cleared). Bypasses the early-out
    // so a cleared (all-1.0) depth shows as solid white here.
    if (pc.dbg.x > 1.5 && pc.dbg.x < 2.5) {
        float zv = clamp(pc.zp.y / (zndc - pc.zp.x), 0.0, 10000.0);
        outAO = vec4(clamp(zv * 0.01, 0.0, 1.0), 0.5, 0.5, 0.5);
        outIL = vec4(0.0);
        return;
    }

    if (zndc >= 0.9999) { outAO = vec4(1.0, 0.5, 0.5, 0.5); outIL = vec4(0.0); return; }   // sky / no prepass coverage (bentN encodes ~0 → receiver uses geomN)

    vec4 C    = fetchPos(uv);
    vec3 cPos = C.xyz;
    vec3 viewV = -normalize(cPos);

    // Normal from depth. Per axis: on a SMOOTH surface both one-sided depth steps
    // are ~equal, so the old `pick the closer side` ternary flipped between the
    // forward and backward difference based on sub-LSB depth quantization → the
    // reconstructed normal ALTERNATED pixel-to-pixel → terraced BANDS (visible in
    // r_ssao_debug 3 on the floor and on distant terrain even though the depth ramp
    // looks smooth — the derivative amplifies quantization the ramp hides). Fix: use
    // a CENTERED difference when smooth (stable, 2-texel baseline averages out the
    // quantization, no flip) and only fall back to the one-sided CLOSER neighbour at
    // a real depth discontinuity (silhouette edge) so edges still don't smear N.
    // Wider 2-texel baseline: depth quantization is ~constant per texel, so a
    // larger neighbour spacing grows the position delta and shrinks the RELATIVE
    // quantization → smoother N on grazing flat ground. The residual AO bands
    // there are the 4-slice visibility integral amplifying a quantization-banded
    // normal (a flat floor's TRUE normal is constant, so any N variation = noise).
    // Costs a touch of N sharpness at small features — fine at half-res.
    const float NB = 3.0;   // wider baseline shrinks the RELATIVE depth quantization in the smooth-case N
    vec4 R = fetchPos(uv + vec2(NB * pc.res.z, 0.0));
    vec4 L = fetchPos(uv - vec2(NB * pc.res.z, 0.0));
    vec4 U = fetchPos(uv + vec2(0.0, NB * pc.res.w));
    vec4 D = fetchPos(uv - vec2(0.0, NB * pc.res.w));
    float dxR = abs(R.w - C.w), dxL = abs(C.w - L.w);
    float dyU = abs(U.w - C.w), dyD = abs(C.w - D.w);
    vec3 ddx = (max(dxR, dxL) > 1.5 * min(dxR, dxL) + 1e-5)
             ? ((dxR < dxL) ? (R.xyz - cPos) : (cPos - L.xyz))
             : (R.xyz - L.xyz) * 0.5;
    vec3 ddy = (max(dyU, dyD) > 1.5 * min(dyU, dyD) + 1e-5)
             ? ((dyU < dyD) ? (U.xyz - cPos) : (cPos - D.xyz))
             : (U.xyz - D.xyz) * 0.5;
    vec3 N = normalize(cross(ddy, ddx));
    if (dot(N, viewV) < 0.0) N = -N;               // face the camera regardless of winding

    // Keep the depth-derivative normal: r_ssao_debug 6 measures how far the
    // G-buffer normal moves it, which is the whole point of the statics pass.
    vec3 nRecon = N;

    // HYBRID: where the normal G-buffer holds a REAL normal, use it instead of
    // the noisy depth-derivative one. Alpha is a SOURCE TAG, not a bare valid
    // flag (see world_normal.frag / shadow_skinned_normal.frag):
    //   1.0 = skinned (NPCs)  — kills the speckle/"dirt" on small curvy geometry
    //   0.5 = static  (level) — the normal the colour pass actually shades with
    //   0.0 = neither         — trees/grass keep the depth-reconstructed N above
    vec4 gbN = texture(uNormal, uv);
    if (gbN.a > 0.25) {
        vec3 rn = gbN.xyz * 2.0 - 1.0;
        // Skip degenerate (zero/near-zero) normals — normalize(0)=NaN stamped
        // black patches; keep the depth-reconstructed N there instead.
        if (dot(rn, rn) > 0.05) {
            rn = normalize(rn);
            // SKINNED ONLY: do NOT flip outward normals (flip negated side/
            // silhouette normals → hard black patches). Instead BEND grazing
            // normals toward the viewer so the half-res depth horizons don't
            // collapse the GTAO integral to 0 on an NPC's edge.
            //
            // STATICS ARE EXCLUDED ON PURPOSE. An unoccluded plane integrates to
            // exactly 1.0 only while N agrees with its OWN horizons; bending N at
            // a grazing floor breaks that agreement and re-creates the residual
            // self-occlusion that r_ssao_bias has been fading away. Feeding the
            // true vertex normal is what makes the fade droppable — bending it
            // here would hand the bands straight back.
            if (gbN.a > 0.75) {
                const float kMinNdV = 0.15;
                float ndv = dot(rn, viewV);
                if (ndv < kMinNdV) rn = normalize(rn + viewV * (kMinNdV - ndv));
            }
            N = rn;
        }
    }

    // r_ssao_debug 3: reconstructed world normal "up-ness" — flat ground ≈ 1,
    // walls ≈ 0.5, noise here means the depth-derivative normals are broken.
    if (pc.dbg.x > 2.5 && pc.dbg.x < 3.5) { outAO = vec4(N.y * 0.5 + 0.5, 0.5, 0.5, 0.5); outIL = vec4(0.0); return; }

    // r_ssao_debug 5: NORMAL SOURCE map — WHERE the real normals landed.
    //   0.0 (black)      = depth-reconstructed (terrain, trees, grass)
    //   0.5 (mid grey)   = statics G-buffer  (r_ssao_static_normals)
    //   1.0 (white)      = skinned G-buffer  (r_ssao_npc_normals)
    // Coverage is a QUESTION ABOUT THE PASS, not about the picture: with the
    // cvar off this view is black except NPCs, with it on every static surface
    // lights up. The readback avg over the frame is the coverage fraction.
    if (pc.dbg.x > 4.5 && pc.dbg.x < 5.5) {
        float src = (gbN.a > 0.75) ? 1.0 : ((gbN.a > 0.25) ? 0.5 : 0.0);
        outAO = vec4(src, 0.0, 0.0, 0.0); outIL = vec4(0.0); return;
    }

    // r_ssao_debug 6: the CORRECTION, in degrees/45 — the angle between the
    // depth-reconstructed normal and the real one, 0 where there is no real
    // normal. This is the 15-25 degree grazing error the statics pass exists to
    // remove, measured instead of eyeballed: readback avg × 45 = mean degrees.
    if (pc.dbg.x > 5.5 && pc.dbg.x < 6.5) {
        float d = (gbN.a > 0.25)
                ? degrees(acos(clamp(dot(normalize(nRecon), N), -1.0, 1.0)))
                : 0.0;
        outAO = vec4(clamp(d / 45.0, 0.0, 1.0), 0.0, 0.0, 0.0); outIL = vec4(0.0); return;
    }

    // Screen axes as world directions — the original's view-space (x,y).
    vec3 rightU = normalize(pc.camRightT.xyz);
    vec3 topU   = normalize(pc.camTopT.xyz);

    const float radius  = pc.zp.z;
    const int   nSample = int(pc.zp.w + 0.5);

    // r_ssao_debug 4: FULLY-OPEN AO — skip the horizon march so no sample can
    // occlude. On a flat floor this MUST be a constant ~1.0; if bands still show
    // here, they come from the reconstructed normal / few-slice integral, not the
    // horizon sampling (decisive split for chasing the residual floor bands).
    bool dbgOpen = (pc.dbg.x > 3.5 && pc.dbg.x < 4.5);

    // Pixels per world unit at this depth → sampling radius in texels.
    float proj_scale    = pc.res.y / (2.0 * pc.camTopT.w);
    float screen_radius = (radius * 0.5 * proj_scale) / C.w;

    // Spatial noise. R4 uses a 4-PHASE diagonal pattern for the radial offset
    // ((y-x)&3 → only 4 discrete sample-ring phases): on large smooth
    // occlusions the rings survive the blur as visible BANDS. Continuous IGN
    // (second channel, +5.588238 decorrelation shift) turns the rings into
    // per-pixel noise the 3×3 blur resolves into a clean gradient.
    ivec2 ip = ivec2(gl_FragCoord.xy);
    // IGN (interleaved-gradient-noise) magic constants, named to this build's
    // provenance (mirror of ogsr::sig) — identical values, just author-bound.
    // (An R2 low-discrepancy alternative was A/B'd 2026-07-01: no visible
    // difference — IGN is also the pattern the 3×3 blur cancels best. Keep IGN.)
    const float IGN_MARIA    = 52.9829189;
    const vec2  IGN_BLUMENAU = vec2(0.06711056, 0.00583715);
    float noiseOffset    = fract(IGN_MARIA * fract(dot(vec2(ip) + 5.588238, IGN_BLUMENAU)));
    float noiseDirection = fract(IGN_MARIA * fract(dot(vec2(ip), IGN_BLUMENAU)));

    // Temporal: rotate the per-pixel slice direction + radial phase by a per-FRAME
    // amount so each frame samples a DIFFERENT set of slices/texels. The blur then
    // EMA-accumulates the frames into a smooth result (kills the 4-slice directional
    // banding that a static pattern can't escape). temporal.y is 0 when r_ssil_temporal
    // is off → these become no-ops and the output is identical to the spatial path.
    noiseDirection = fract(noiseDirection + pc.temporal.y);
    noiseOffset    = fract(noiseOffset    + pc.temporal.y * 0.61803399);

    float falloff_mul   = 2.0 / (radius * radius);
    vec2  screen_res_mul = (1.0 / float(nSample)) * pc.res.zw;
    float pi_by_slices  = PI / float(SLICES);

    float visibility = 0.0;

    // SSIL: accumulate the colour of horizon-raising occluders (the surfaces that
    // actually bounce light at this pixel), read from the PREVIOUS frame's lit
    // scene. ilOn gates the prev-colour taps so r_ssil 0 costs nothing extra.
    bool  ilOn      = pc.dbg.y > 0.5;
    float fireClamp = pc.dbg.z;
    vec3  ilAccum   = vec3(0.0);
    float ilWsum    = 0.0;

    for (int slice = 0; slice < SLICES; ++slice)
    {
        float phi  = (float(slice) + noiseDirection) * pi_by_slices;
        vec2 omega = vec2(cos(phi), sin(phi));
        vec3 directionV = rightU * omega.x + topU * omega.y;

        vec3 orthoDirectionV = directionV - dot(directionV, viewV) * viewV;
        vec3 axisV       = cross(directionV, viewV);
        vec3 projNormalV = N - axisV * dot(N, axisV);
        float projNormalLength = length(projNormalV);

        float sgnN  = sign(dot(orthoDirectionV, projNormalV));
        float cosN  = clamp(dot(projNormalV, viewV) / projNormalLength, 0.0, 1.0);
        float n     = sgnN * fast_acos(cosN);
        float sinN2 = 2.0 * sin(n);

        for (int side = 0; side < 2; ++side)
        {
            float sideSign = -1.0 + 2.0 * float(side);
            float cHorizonCos = -1.0;

            for (int samp = 0; samp < nSample && !dbgOpen; ++samp)
            {
                // Min offset: R4 uses 4+sample FULL-res texels; we run at half
                // res, so 2+sample keeps the same world-space footprint — the
                // nearest taps are what catch tight contact occlusion.
                vec2 s = max(vec2(screen_radius * (float(samp) + noiseOffset)),
                             vec2(2.0 + float(samp))) * screen_res_mul;
                vec2 sTexCoord = uv + sideSign * s * vec2(omega.x, -omega.y);
                vec3 sPos = fetchPos(sTexCoord).xyz;
                vec3 sHorizonV = sPos - cPos;
                float d2 = dot(sHorizonV, sHorizonV);
                float falloff = clamp(d2 * falloff_mul, 0.0, 1.0);
                float H = dot(sHorizonV * inversesqrt(max(d2, 1e-12)), viewV);
                if (H > cHorizonCos) {
                    // This sample raised the horizon → it's a visible occluder
                    // that bounces light. Accumulate its colour as a WEIGHTED
                    // AVERAGE (range falloff × cosine to the receiver normal) — a
                    // proper average, NOT a sum, so a single near sample can't
                    // blow it up and flat ground (every neighbour is an "occluder")
                    // doesn't glow.
                    if (ilOn) {
                        vec3  dirS = sHorizonV * inversesqrt(max(dot(sHorizonV, sHorizonV), 1e-6));
                        float w    = (1.0 - falloff) * max(dot(dirS, N), 0.0);
                        vec3  sc   = textureLod(uPrevColor, sTexCoord, 0.0).rgb;
                        ilAccum += min(sc, vec3(fireClamp)) * w;
                        ilWsum  += w;
                    }
                    cHorizonCos = mix(H, cHorizonCos, falloff);
                }
            }

            float h = n + clamp(sideSign * fast_acos(cHorizonCos) - n, -PI * 0.5, PI * 0.5);
            visibility += projNormalLength * (cosN + h * sinN2 - cos(2.0 * h - n)) * 0.25;
        }
    }

    float aoOut = clamp(visibility / float(SLICES), 0.0, 1.0);

    // Grazing-angle fade (pc.camDir.w = r_ssao_bias = the N·V threshold). The
    // depth-reconstructed normal is unreliable at grazing angles, so a flat open
    // surface fails to cancel its OWN horizon there → residual self-occlusion
    // (AO < 1) that pow(ao, r_ssao_strength) blows into the floor "bands". Where
    // the surface faces away from the viewer (N·V → 0) we fade AO back to fully
    // open; head-on surfaces (reliable recon, real contact shade) keep full AO.
    // This kills the bands at ANY strength because it removes the bogus AO at the
    // source instead of fighting the pow. (Horizon/elevation biases couldn't: the
    // open-side horizon is clamp-saturated, so biasing it is a no-op on flat ground.)
    float ndv       = clamp(dot(N, viewV), 0.0, 1.0);
    float grazeFade = smoothstep(pc.camDir.w, pc.camDir.w + 0.25, ndv);
    aoOut = mix(1.0, aoOut, grazeFade);

    // No bent normal anymore: it was retired (env_common gtaoBentN → geomN, see
    // the полосы saga) and the target is R16F now — dropping its per-slice
    // arc-bisector math and the RGBA payload is the free perf half of that call.
    outAO = vec4(aoOut, 0.0, 0.0, 0.0);

    // IL = weighted AVERAGE of the occluders' colour (the light bouncing toward
    // this pixel). No occlusion gate here — gating by (1-AO) concentrated the
    // bounce into a bright rim at contacts (a "neon underglow"). Energy is handled
    // at composite instead (fill shadows, not lit surfaces). Where there are no
    // occluders (open sky-facing ground) ilWsum≈0 → IL 0. Composite applies the
    // shadow gate + r_ssil_strength + exposure.
    // Bake r_ssil_strength (dbg.w) here so the forward receivers apply a fixed
    // ssilBoost() — keeps the strength a single live knob without a UBO field.
    vec3 ilAvg = (ilWsum > 1e-3) ? ilAccum / ilWsum : vec3(0.0);
    outIL = vec4(ilAvg * pc.dbg.w, 1.0);
}
