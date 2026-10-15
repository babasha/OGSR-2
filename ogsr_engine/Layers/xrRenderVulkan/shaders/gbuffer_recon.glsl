#ifndef GBUFFER_RECON_GLSL
#define GBUFFER_RECON_GLSL
// xrRenderVulkan — WHERE AND WHICH WAY IS THIS PIXEL, from the prepass depth and
// the normal G-buffer. One copy, because RTAO and the final gather both start
// every ray from this answer and a drift between them would look like a bug in
// whichever of the two was looked at second.
//
// Requires the includer to declare, before the #include:
//   sampler2D uDepth    full-res prepass depth (D32), NEAREST sampler
//   sampler2D uNormal   normal G-buffer: rgb = worldN*0.5+0.5, a = source tag
//                       (1 skinned, 0.5 static, 0 none)
// and a push-constant block named `pc` carrying:
//   camDir.xyz   camera forward (unit)
//   camRightT.xyz / camTopT.xyz   right/top already scaled by tan(fov/2)
//   zp.x = proj _33, zp.y = proj _43   (zview = zp.y / (zndc - zp.x))
//   res.zw = 1 / target size

// Camera-relative world position + view depth, reconstructed at the CENTRE of the
// full-res depth texel the NEAREST sampler picks — same rule as ssao.frag: the
// prepass rasterized that depth at the texel centre, so a centre ray lands ON the
// surface instead of sliding off it by the sub-texel mismatch.
vec4 fetchPos(vec2 uv)
{
    vec2 fs = vec2(textureSize(uDepth, 0));
    vec2 tc = (clamp(floor(uv * fs), vec2(0.0), fs - 1.0) + 0.5) / fs;
    float zndc  = texture(uDepth, tc).r;
    float zview = clamp(pc.zp.y / (zndc - pc.zp.x), 0.0, 10000.0);
    vec2  ndc   = vec2(tc.x * 2.0 - 1.0, 1.0 - 2.0 * tc.y);   // D3D ndc (y up)
    vec3  ray   = pc.camDir.xyz + pc.camRightT.xyz * ndc.x + pc.camTopT.xyz * ndc.y;
    return vec4(ray * zview, zview);
}

// Normal: prefer the G-buffer (statics + skinned write real vertex normals),
// fall back to a centred depth derivative for what does not (trees, grass,
// terrain). Same source tag GTAO reads: 1 skinned, 0.5 static, 0 none.
//
// The derivative picks the SHORTER of the two one-sided differences whenever they
// disagree by more than half — across a silhouette the long side spans a depth
// discontinuity and would tilt the normal into the background.
vec3 reconNormal(vec2 uv, vec4 C, vec3 viewV)
{
    const vec3 cPos = C.xyz;
    vec3 N;
    vec4 gbN = texture(uNormal, uv);
    if (gbN.a > 0.25 && dot(gbN.xyz - 0.5, gbN.xyz - 0.5) > 1e-6) {
        N = normalize(gbN.xyz * 2.0 - 1.0);
    } else {
        const float NB = 2.0;
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
        N = normalize(cross(ddy, ddx));
    }
    if (dot(N, viewV) < 0.0) N = -N;    // face the camera regardless of winding
    return N;
}

#endif // GBUFFER_RECON_GLSL
