#version 450
// xrRenderVulkan — screen-space motion vectors (Phase 1: camera + static world).
//
// Foundation for DLSS/FSR upscaling, frame-gen and (later) the path-tracer
// denoiser temporal accumulation. Runs fullscreen right after the depth prepass
// (next to GTAO), reading ONLY the scene depth — no per-geometry shader changes.
//
// We reconstruct each pixel's camera-relative WORLD position from depth using
// the exact same frustum-ray basis the GTAO pass uses (DeriveProjTerms:
// camDir + right·tanX·ndc.x + top·tanY·ndc.y, zview = _43/(zndc−_33)), add the
// camera eye to get the absolute world position, then re-project it with the
// PREVIOUS frame's view-projection. The screen-space delta (prevUV − curUV) is
// the motion vector.
//
// Phase 1 captures camera motion + everything statics see from it (the dominant
// motion source). Self-moving geometry (skinned NPCs, swaying trees, wind grass)
// reads 0 here and gets overwritten by a dynamics MV pass in Phase 2.
//
// Output: RG16F = (prevUV − curUV) in [0,1] UV space (resolution-independent).
// Sky / uncovered pixels (cleared depth) output 0.

layout(location = 0) out vec2 outMV;

layout(set = 0, binding = 0) uniform sampler2D uDepth;   // full-res scene depth (D32, prepass)

layout(push_constant) uniform PC {
    vec4 camDir;     // xyz = camera forward (unit),       w = eye.x
    vec4 camRightT;  // xyz = right * tan(fovX/2),         w = eye.y
    vec4 camTopT;    // xyz = top   * tan(fovY/2),         w = eye.z
    vec4 zp;         // x = proj _33, y = proj _43, z = 1/width, w = 1/height
    mat4 prevVP;     // previous-frame view-proj, UNJITTERED (row-major Fmatrix → GLSL reads as transpose)
    vec4 jitter;     // xy = this frame's sub-pixel jitter in D3D-NDC (0 when DLSS off)
} pc;

void main()
{
    vec2  uv   = gl_FragCoord.xy * pc.zp.zw;
    float zndc = texture(uDepth, uv).r;

    // JITTER-FREE MV (Option A): the depth was rasterised at the JITTERED projection,
    // so this pixel sits at the jittered NDC. The same surface projects to
    // (ndcPix − jitter) under the UNJITTERED view-proj — reconstruct + reproject in
    // that unjittered space so the motion vector carries NO sub-pixel jitter wobble
    // (DLSS applies the jitter itself from the offset we pass to Evaluate). The camDir/
    // right/top basis is derived from the UNJITTERED cur view-proj on the C++ side.
    vec2 ndcPix = vec2(uv.x * 2.0 - 1.0, 1.0 - 2.0 * uv.y);   // D3D ndc (y up) at the jittered pixel
    vec2 ndc    = ndcPix - pc.jitter.xy;                       // unjittered ndc for this surface
    vec3 ray = pc.camDir.xyz + pc.camRightT.xyz * ndc.x + pc.camTopT.xyz * ndc.y;
    vec3 eye = vec3(pc.camDir.w, pc.camRightT.w, pc.camTopT.w);

    // Re-project with the PREVIOUS frame's view-proj → previous clip.
    vec4 pclip;
    if (zndc >= 0.9999) {
        // Sky / no prepass coverage: a point at INFINITY along the ray. Project as a
        // direction (w=0) so the prev view-proj's translation column is ignored —
        // there's no parallax at infinity, only camera ROTATION moves the sky.
        pclip = pc.prevVP * vec4(ray, 0.0);
    } else {
        // Solid geometry: reconstruct the world position from depth, reproject it.
        float zview = clamp(pc.zp.y / (zndc - pc.zp.x), 0.0, 100000.0);
        pclip = pc.prevVP * vec4(eye + ray * zview, 1.0);
    }

    if (abs(pclip.w) <= 1e-6) { outMV = vec2(0.0); return; }
    vec2 pndc   = pclip.xy / pclip.w;
    vec2 prevUV = vec2(pndc.x * 0.5 + 0.5, 0.5 - 0.5 * pndc.y);   // inverse of the ndc map above

    // Current (unjittered) position of this surface — NOT the jittered pixel uv.
    vec2 curUV = vec2(ndc.x * 0.5 + 0.5, 0.5 - 0.5 * ndc.y);
    outMV = prevUV - curUV;   // where the pixel WAS minus where it IS (UV space), jitter-free
}
