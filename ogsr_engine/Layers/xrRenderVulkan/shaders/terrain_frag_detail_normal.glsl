// Component of world_terrain.frag.glsl — textual include, not a standalone shader.
// Uses the set-0 uDn* samplers and the vDetailUV/vWorldPos inputs of the parent.

// ---- Detail NORMAL MAPPING (R4 CBlender_BmmD s_dn_*). The primary ground-relief
// mechanism on terrain. Blends the 4 per-channel tangent normals by the splat
// mask, then rotates into world space via a screen-space cotangent frame (terrain
// has no per-vertex tangents). Returns geomN unchanged when off.
vec3 detailNormal(vec3 geomN, vec2 uvs[4], vec4 mask, float strength, out float outCav, out float outGloss)
{
    outCav = 0.0; outGloss = 0.0;
    if (strength <= 0.0) return geomN;
    // One sample per channel AT THAT CHANNEL'S OWN parallaxed uv; decode normal
    // (R4: n = tex.wzy*2-1) AND gloss (R4: gloss = tex.r), blended by the mask.
    vec4 sR = texture(uDnR, uvs[0], L.spot_flash.z), sG = texture(uDnG, uvs[1], L.spot_flash.z);   // DLSS mip bias (0 native)
    vec4 sB = texture(uDnB, uvs[2], L.spot_flash.z), sA = texture(uDnA, uvs[3], L.spot_flash.z);
    vec3 n = (sR.wzy * 2.0 - 1.0) * mask.r
           + (sG.wzy * 2.0 - 1.0) * mask.g
           + (sB.wzy * 2.0 - 1.0) * mask.b
           + (sA.wzy * 2.0 - 1.0) * mask.a;
    outGloss = sR.x * mask.r + sG.x * mask.g + sB.x * mask.b + sA.x * mask.a;
    n.xy *= strength;
    n = normalize(n);
    // Cavity: how far the micro-normal tilts off the surface (drives micro AO).
    outCav = clamp(1.0 - n.z, 0.0, 1.0);

    // Frame from the UNparallaxed detail uv (stable derivatives; the frame is the
    // uv axes' world orientation - unaffected by the small per-channel offsets).
    vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
    vec2 du1 = dFdx(vDetailUV), du2 = dFdy(vDetailUV);
    vec3 dp2p = cross(dp2, geomN), dp1p = cross(geomN, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = inversesqrt(max(dot(T, T), dot(B, B)));
    T *= inv; B *= inv;
    return normalize(T * n.x + B * n.y + geomN * n.z);
}
