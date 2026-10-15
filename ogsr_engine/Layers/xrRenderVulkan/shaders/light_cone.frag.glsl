#version 450
// xrRenderVulkan — per-light volumetric cone (REAL directional light beams).
//
// Replaces the R4 `models\lightplanes` texture-sheet fakes: for every spot
// light the game flags volumetric (headlights, searchlights, pole lamps) we
// raymarch an analytic cone of scattering media from the light's own params
// (position / direction / cone angle / range / colour) and ADD the in-scatter
// over the HDR scene. Angle-independent by construction — no more "sheets you
// only see side-on".
//
// One fullscreen-triangle draw per light (shares sunshafts.vert). The ray is
// clipped to the cone's bounding sphere (early discard for pixels that miss)
// and to the scene depth, then N jittered steps accumulate a radial+axial
// falloff density. Forward-scattering glare boosts the beam when looking into
// the light. The budget-owning beam is also cut per step by the spot+grass
// shadow map (fences / crowns / grass carve the visible cone), with a
// golden-angle disc jitter as an area-light penumbra (r_light_cone_soft).

layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDepth;      // scene depth (D32)
layout(set = 0, binding = 1) uniform sampler2D uSpotShadow;  // spot shadow map (the budget pick)

layout(push_constant) uniform PC {
    vec4 camPos;      // xyz = camera position,      w = proj _33
    vec4 camDir;      // xyz = camera forward,       w = proj _43
    vec4 camRightT;   // xyz = right * tan(fovX/2),  w = density (r_light_cone_density)
    vec4 camTopT;     // xyz = up * tan(fovY/2),     w = glare boost
    vec4 lightPos;    // xyz = cone apex,            w = beam length (m)
    vec4 lightDir;    // xyz = cone axis (unit),     w = tan(half angle)
    vec4 lightCol;    // rgb = colour × intensity (eye-fade premultiplied),
                      // w < 99.5 = shadow-tap soft radius in texels (r_light_cone_soft,
                      // area-light penumbra for grass/crown cutouts), else debug index+100
    vec4 beamPrm;     // x = apex (lamp) radius (m) — FRUSTUM; y = this beam's spot-pool
                      // TILE + 1 (0 = no shadow; per-step occlusion: fences/trees/NPC/
                      // grass cut the beam); z = spot far plane (range) for the
                      // linear-depth compare; w > 0 = SYNTH beam: lamp-face glow only
                      // (full 15 cm, exp(-d/w) after) — the LONG beam comes from the fog
    mat4 spotVP;      // this light's TILE view·proj (raw Fmatrix = row-vector transform)
} pc;

void main()
{
    // Scene depth → view-space Z → world point (frustum-ray basis, see sunshafts).
    float zndc  = texture(uDepth, vUV).r;
    float zview = clamp(pc.camDir.w / (zndc - pc.camPos.w), 0.0, 10000.0);
    vec2  ndcXY = vec2(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * vUV.y);   // D3D ndc (y up)
    vec3  ray   = pc.camDir.xyz + pc.camRightT.xyz * ndcXY.x + pc.camTopT.xyz * ndcXY.y;
    vec3  ro    = pc.camPos.xyz;
    float rayLen = length(ray);
    vec3  rd     = ray / rayLen;
    float tScene = zview * rayLen;     // distance to the opaque scene along rd

    vec3  A    = pc.lightPos.xyz;
    vec3  D    = pc.lightDir.xyz;
    float len  = pc.lightPos.w;
    float tanH = pc.lightDir.w;
    float r0   = pc.beamPrm.x;   // lamp-face radius: beam is a FRUSTUM, not a point cone

    // Bounding sphere of the cone — cheap reject for pixels whose ray misses it.
    float baseR = r0 + len * tanH;
    vec3  C  = A + D * (len * 0.5);
    float R  = sqrt(len * len * 0.25 + baseR * baseR) * 1.02;
    vec3  oc = ro - C;
    float b  = dot(oc, rd);
    float cc = dot(oc, oc) - R * R;
    float h  = b * b - cc;
    if (h <= 0.0) discard;
    h = sqrt(h);
    float t0 = max(-b - h, 0.0);
    float t1 = min(-b + h, tScene);

    if (t1 <= t0) discard;

    // Jittered march (interleaved gradient noise kills the step banding).
    const int STEPS = 24;
    bool  useShadow = pc.beamPrm.y > 0.5;
    float stepLen = (t1 - t0) / float(STEPS);
    float dith = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float t   = t0 + stepLen * (0.5 + (dith - 0.5) * 0.9);
    float acc = 0.0;
    // Area-light penumbra for the per-step occlusion tap: the spot map resolves
    // grass blades at millimetre texels, so a point-sampled compare paints razor
    // "threads" through a grass field. Jitter each step's tap on a golden-angle
    // disc (radius in texels, r_light_cone_soft) — 24 steps integrate to a soft
    // penumbra, no temporal history needed and nothing shimmers (dith is static
    // per pixel). w >= 99.5 is the debug-index encoding → no softening there.
    float softUV = (pc.lightCol.w < 99.5)
                 ? pc.lightCol.w / float(textureSize(uSpotShadow, 0).x) : 0.0;
    for (int i = 0; i < STEPS; ++i, t += stepLen) {
        vec3  p = ro + rd * t;
        vec3  q = p - A;
        float m = dot(q, D);                     // axial coordinate (m from the apex)
        if (m <= 0.0 || m >= len) continue;
        float rc = r0 + m * tanH;                // frustum radius at this axial depth
        float rr = length(q - D * m);            // radial distance off the axis
        float x  = rr / max(rc, 1e-4);
        if (x >= 1.0) continue;
        // Per-step occlusion vs the spot shadow map: fences / tree crowns / NPC
        // between the lamp and this point CUT the visible beam. LINEAR-depth
        // compare with a world epsilon (same reasoning as shadow_common.glsl —
        // a constant NDC bias leaks through fences near the far plane).
        if (useShadow) {
            vec4 sc = pc.spotVP * vec4(p, 1.0);
            if (sc.w > 0.0) {
                vec3 sn = sc.xyz / sc.w;
                vec2 suv = sn.xy * 0.5 + 0.5;
                suv.y = 1.0 - suv.y;
                if (softUV > 0.0) {
                    float ang = 6.2831853 * fract(dith + float(i) * 0.61803399);
                    float rad = softUV * sqrt(fract(dith * 7.13 + float(i) * 0.7548));
                    suv += vec2(cos(ang), sin(ang)) * rad;
                }
                if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0 && sn.z <= 1.0) {
                    // Atlas tile rect (4x2 pool, beamPrm.y = tile+1), inset one
                    // texel so the jittered tap never reads a neighbour tile.
                    int  tile = int(pc.beamPrm.y + 0.5) - 1;
                    const vec2 kTileScale = vec2(0.25, 0.5);
                    vec2 tBase = vec2(float(tile & 3), float(tile >> 2)) * kTileScale;
                    vec2 atx   = 1.0 / vec2(textureSize(uSpotShadow, 0));
                    vec2 auv   = clamp(tBase + suv * kTileScale,
                                       tBase + atx, tBase + kTileScale - atx);
                    const float sN = 0.5;   // ComputeSpotVPFor near plane
                    float sF   = max(pc.beamPrm.z, 1.0);
                    float zRef = sN * sF / max(sF - sn.z * (sF - sN), 1e-4);
                    float zMap = sN * sF / max(sF - texture(uSpotShadow, auv).r * (sF - sN), 1e-4);
                    if (zRef - 0.08 > zMap) continue;
                }
            }
        }
        float radial = 1.0 - x * x;              // soft-edged cross-section
        // Axial profile: game lights keep the classic full-length falloff;
        // SYNTH beams (beamPrm.w > 0) show only a lamp-face glow — full within
        // 15 cm, exponential to transparent after (the fog draws the long beam).
        float axial;
        if (pc.beamPrm.w > 0.0)
            axial = exp(-max(m - 0.15, 0.0) / pc.beamPrm.w);
        else {
            axial = 1.0 - m / len;
            axial *= axial;
        }
        acc += radial * radial * axial;
    }
    acc *= stepLen;                              // Riemann sum → metres of media

    // Forward-scattering glare: the beam flares when you look INTO the light
    // (rd against the axis), like driving toward oncoming headlights.
    float glare = 1.0 + pc.camTopT.w * pow(max(dot(D, -rd), 0.0), 6.0);

    // PARTICIPATING MEDIA, not plain additive: the beam both EMITS in-scatter
    // and EXTINGUISHES the background (out = S + bg·T, blend ONE/SRC_ALPHA with
    // T in alpha). Pure additive drowned in the daylit scene (bg sits on the
    // Reinhard shoulder, +0.2 HDR moves nothing); extinction pulls the pixel
    // TOWARD the beam colour, so it reads day (lit haze) and night (glow) both.
    // Beam luminance (was const kBeamLum 2.2) now rides premultiplied into
    // lightCol.rgb — live-tunable via r_light_cone_lum.
    float tau = acc * pc.camRightT.w;            // optical thickness (density cvar)
    float T   = exp(-tau);

    // DEBUG (r_light_cones 2 → C++ pushes lightCol.w = cone index + 100):
    // false-colour the MARCHED cone itself, boosted and depth-clamped — a
    // bright solid cone must sit exactly on the lamp/headlight. Nothing shown
    // at a listed light = the march/geometry side; right shape wrong place =
    // ray-basis side (compare against the r_light_debug centre-ray self-test).
    if (pc.lightCol.w >= 99.5) {
        const vec3 dbgCol[8] = vec3[8](
            vec3(1,0,0), vec3(0,1,0), vec3(0.2,0.4,1), vec3(1,1,0),
            vec3(1,0,1), vec3(0,1,1), vec3(1,1,1), vec3(1,0.5,0));
        vec3 cc2 = dbgCol[int(pc.lightCol.w - 99.5) & 7];
        if (acc <= 0.001) discard;
        outColor = vec4(cc2 * min(acc * 6.0, 4.0), max(T, 0.3));
        return;
    }

    vec3 S = pc.lightCol.rgb * ((1.0 - T) * glare);

    outColor = vec4(S, T);
}
