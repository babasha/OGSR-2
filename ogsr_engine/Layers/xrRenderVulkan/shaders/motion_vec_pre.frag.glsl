#version 450
// xrRenderVulkan — MOTION-VECTOR RESOLVE for the prepass-written field (r_mv_prepass).
//
// The depth prepass now carries an RG16F attachment cleared to a SENTINEL. Geometry
// that moves under its own power and already runs a fragment shader there (tree
// crowns, for the leaf alpha-test) writes its TRUE screen motion in the same raster
// that lays its depth; statics bind the same pipelines with colorWriteMask 0 and
// leave the sentinel standing. This pass reads that field and fills every sentinel
// texel by reconstruction from depth + the previous view-proj — the arithmetic of
// motion_vec.frag, kept line-for-line so the two paths cannot drift.
//
// WHY a separate pass instead of letting the fullscreen reconstruction run later and
// be overdrawn by overlays: the AO temporal history needs the motion field for THIS
// frame's pixels, and the SSAO pass runs immediately after the prepass. Resolving
// here makes the field complete (statics by reconstruction, dynamics by their own
// raster) before its first consumer, and deletes the dynamics' second raster.
//
// Output: RG16F = (prevUV − curUV) in [0,1] UV space, same convention as everywhere.

layout(location = 0) out vec2 outMV;

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // full-res scene depth (D32, prepass)
layout(set = 0, binding = 1) uniform sampler2D uPreMV;   // the prepass RG16F field (sentinel where nothing wrote)

layout(push_constant) uniform PC {
    vec4 camDir;     // xyz = camera forward (unit),       w = eye.x
    vec4 camRightT;  // xyz = right * tan(fovX/2),         w = eye.y
    vec4 camTopT;    // xyz = top   * tan(fovY/2),         w = eye.z
    vec4 zp;         // x = proj _33, y = proj _43, z = 1/width, w = 1/height
    mat4 prevVP;     // previous-frame view-proj, UNJITTERED (row-major Fmatrix → GLSL reads as transpose)
    vec4 jitter;     // xy = this frame's sub-pixel jitter in D3D-NDC (0 when DLSS off); z = sentinel test threshold
} pc;

void main()
{
    vec2 uv = gl_FragCoord.xy * pc.zp.zw;

    // A texel a dynamic raster claimed keeps its own motion. The sentinel is a value
    // no real motion vector can reach (a full-screen sweep is well under one UV unit),
    // so a single compare on .x separates "written" from "cleared" without a mask
    // attachment. NEAREST sampling — this is a full-res 1:1 read, and a filtered tap
    // across the silhouette would blend the sentinel into a real vector.
    vec2 pre = texture(uPreMV, uv).rg;
    if (pre.x > pc.jitter.z) { outMV = pre; return; }

    float zndc = texture(uDepth, uv).r;

    // JITTER-FREE MV (Option A) — see motion_vec.frag for the full argument: the depth
    // was rasterised at the JITTERED projection, so unjitter this pixel's ndc before
    // reconstructing, and reproject in that same unjittered space.
    vec2 ndcPix = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);
    vec2 ndc    = ndcPix - pc.jitter.xy;
    vec3 ray = pc.camDir.xyz + pc.camRightT.xyz * ndc.x + pc.camTopT.xyz * ndc.y;
    vec3 eye = vec3(pc.camDir.w, pc.camRightT.w, pc.camTopT.w);

    vec4 pclip;
    if (zndc >= 0.9999) {
        // Sky / no prepass coverage: a point at INFINITY along the ray (w = 0) — no
        // parallax out there, only camera rotation moves it.
        pclip = pc.prevVP * vec4(ray, 0.0);
    } else {
        float zview = clamp(pc.zp.y / (zndc - pc.zp.x), 0.0, 100000.0);
        pclip = pc.prevVP * vec4(eye + ray * zview, 1.0);
    }

    if (abs(pclip.w) <= 1e-6) { outMV = vec2(0.0); return; }
    vec2 pndc   = pclip.xy / pclip.w;
    vec2 prevUV = vec2(pndc.x * 0.5 + 0.5, 0.5 - 0.5 * pndc.y);
    vec2 curUV  = vec2(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y);
    outMV = prevUV - curUV;
}
