#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — FORWARD CROWN CUBE TIER, vertex stage (r_tree_vox_dist).
//
// The middle rung of the tree LOD ladder: mesh → THESE CUBES → FLOD billboard.
// Past r_tree_vox_dist a crown's alpha-tested leaf cards are cut from both the
// colour cull and the depth prepass and this draws the baked voxel cloud in their
// place — one opaque cube per occupied crown cell, colour baked in, no texture
// fetch and no `discard` anywhere in the tier.
//
// NO VERTEX INPUT. Everything is pulled from set 0:
//   b3 = the baked cloud (VK::GpuTreeVoxel), indexed by gl_VertexIndex / 18
//   b4 = the per-draw payload tree_cull.comp produced (VK::GpuVoxDraw), indexed by
//        gl_InstanceIndex — the command's firstInstance IS its own slot, the same
//        "address the payload through firstInstance" trick tree_vsm_vox uses for
//        page routing. That is what lets the entire tier be ONE
//        vkCmdDrawIndirectCount instead of a CPU loop over 193 k instances.
//
// 18 VERTICES PER CUBE, NOT 36: only the three camera-facing faces are emitted, and
// those cover the cube's silhouette exactly (the other three are back faces the
// rasterizer would discard anyway — but only AFTER paying for their vertices).
//
// ⭐ THE WHOLE SHADE HAPPENS HERE, NOT IN THE FRAGMENT SHADER, and that is the tier's
// entire economic argument. A crown is hundreds of unsorted opaque boxes: its fragment
// count runs several times its vertex count once overdraw is counted, and the cubes are
// deliberately NOT in the depth prepass, so early-Z only helps within the pass. Putting
// the sky lookup, the probe field and the VSM atlas walk per PIXEL there made the tier
// cost more than the leaf-card mesh it replaces (measured 24-08: Trees 8.0 -> 11.8 ms
// as the handover was pulled from off to 15 m). Per cube the same shade is amortised
// over the box's whole screen footprint. The face normal still varies per face, so the
// facets read as facets — `flat` picks each triangle's own provoking vertex.
//
// Wind is applied ONCE at the cube centre so the cube sways rigidly, with the same
// ssfxTreeWind the crown mesh uses (tc_y 0.35) — the cloud tracks the sway of the
// trees it is standing in for.

#include "tree_instance.glsl"   // TreeInstance + XformBuf (set 0 b0) — VK::GpuTreeInstance
#define VSM_SET 2               // EnvLight is bound at set 2 for trees
#define ENV_SET 2
#define VSM_GRASS_DIRECT        // world-space atlas lookup — the mask is wrong here (see below)
#include "vsm_sample.glsl"
#include "light_ubo.glsl"       // Lighting UBO (set 2 b0) — eye, sun, fog, ambient
#include "sky_ambient.glsl"     // skyAmbientUp — the crown's sky fill, unchanged
#include "ddgi_field.glsl"      // ddgiFieldWeighted — the vertex-callable probe lookup
#include "tree_gfx_push.glsl"   // TreeGfxPush (160 B) — shared with the crown mesh
#include "ssfx_tree_wind.glsl"

// Mirrors VK::GpuTreeVoxel (16 B): centre in species-local mesh space + packed
// colour (rgb = baked foliage tint with AO and height grading, high nibble of a =
// size jitter, low nibble = leaf coverage for the shadow caster's sparsification).
struct Voxel { vec3 p; uint c; };
layout(set = 0, binding = 3, std430) readonly buffer VoxBuf { Voxel vox[]; };

// Mirrors VK::GpuVoxDraw (16 B) — written by tree_cull.comp's voxEmit().
struct VoxDraw { uint tree; float cubeSize; float density; float fade; };
layout(set = 0, binding = 4, std430) readonly buffer VoxDrawBuf { VoxDraw voxDraw[]; };

layout(location = 0) out flat vec3  vColor;   // fully shaded, fogged — the FS only dithers
layout(location = 1) out flat float vFade;    // screen-door: >0 dissolve OUT, <0 IN

// Two triangles of a unit quad in the face's own plane.
const vec2 kQuad[6] = vec2[6](
    vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5),
    vec2(-0.5, -0.5), vec2(0.5,  0.5), vec2(-0.5, 0.5));

void killVert()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);   // behind the far plane — clipped away
    vColor = vec3(0.0); vFade = 0.0;
}

void main()
{
    VoxDraw d  = voxDraw[gl_InstanceIndex];
    uint    vi = uint(gl_VertexIndex) / 18u;              // global voxel index
    int     lv = gl_VertexIndex - int(vi) * 18;           // 0..17

    // CONTINUOUS density thinning (UE grows voxels per DAG level with ~4x fewer per
    // level; we do it smoothly): as the rendered cube outgrows this grid's cell, a
    // matching fraction of cubes is culled — density = (cell/cube)^2, so the crown's
    // covered area stays constant and the cube COUNT is continuous in distance,
    // converging to one or two crown-sized blocks instead of a pile of overlapping
    // cubes. The kill decision hashes the STABLE voxel index (golden-ratio low
    // discrepancy), so cubes vanish one by one and never reshuffle.
    if (d.density < 1.0) {
        if (fract(float(vi) * 0.6180339887) >= d.density) { killVert(); return; }
    }

    Voxel        V = vox[vi];
    TreeInstance t = inst[d.tree];

    vec4  c4    = t.xform * vec4(V.p, 1.0);
    float baseY = t.xform[3].y;
    float H     = c4.y - baseY;
    vec3  centre = c4.xyz + ssfxTreeWindWorld(c4.xyz, H, 0.35, t._p0, baseY,
                                              pc.wind_params, pc.wsetup_trees, pc.wind_anim);

    // Instance scale (uniform in tree xforms) scales the cube edge; the per-voxel
    // size jitter (baked hash HIGH nibble, 0.75-1.2) breaks the minecraft-regular
    // grid reading that a constant edge produces.
    float sc   = length(t.xform[0].xyz);
    float jit  = 0.75 + 0.45 * float((V.c >> 28) & 0xFu) / 15.0;
    float edge = d.cubeSize * sc * jit;

    // Three camera-facing faces. sign() is avoided on purpose: it returns 0 when the
    // camera sits exactly on a face plane, which would collapse that face to a sliver.
    vec3 toEye = L.eye_pos.xyz - centre;
    vec3 s = vec3(toEye.x >= 0.0 ? 1.0 : -1.0,
                  toEye.y >= 0.0 ? 1.0 : -1.0,
                  toEye.z >= 0.0 ? 1.0 : -1.0);
    int  face = lv / 6;
    vec2 q    = kQuad[lv - face * 6];
    vec3 off, N;
    if      (face == 0) { off = vec3(s.x * 0.5, q.x, q.y); N = vec3(s.x, 0.0, 0.0); }
    else if (face == 1) { off = vec3(q.x, s.y * 0.5, q.y); N = vec3(0.0, s.y, 0.0); }
    else                { off = vec3(q.x, q.y, s.z * 0.5); N = vec3(0.0, 0.0, s.z); }

    vec3 wp     = centre + off * edge;
    gl_Position = pc.mViewProj * vec4(wp, 1.0);
    vFade       = d.fade;

    // ---- Shade, at the cube CENTRE (the lookups) and per FACE (the sun's shape). ----
    // Same per-tree openness the crown mesh lights with (tree.vert's hemiFac), so the
    // handover cannot move the exposure — only the geometry.
    float hemi = clamp(t.c_scale_hemi + t.c_bias_hemi, 0.3, 1.5);

    // Sun. Colour and the 0.88 crown factor are tree.vert's vSunLit verbatim.
    vec3 sunPart = pc.vSunColor.rgb * hemi * 0.88;
    if (dot(sunPart, sunPart) > 0.0) {
        // ⚠ The VSM screen-space MASK is unusable for this tier: it is resolved at
        // PREPASS depth and these crowns were cut from the prepass, so at a cube pixel
        // it describes whatever stands BEHIND the crown — exactly the failure
        // vsm_sample.glsl documents for grass (terrain shadows painting themselves onto
        // a bush). The direct world-space atlas walk is the same fix, at the cube centre.
        // With VSM off there is no world-space sun lookup on this set at all; the
        // cascade path is legacy and copying tree.frag's private sunShadow1 into a third
        // place is the drift this codebase has been bitten by, so the tier renders
        // unshadowed there and r_vsm 0 is a debug mode.
        float sh = (L.shadow_params.w > 0.5) ? vsmSunShadowGrassDirect(centre, L.spot_params.w)
                                             : 1.0;
        // MEAN-PRESERVING shape term. Half-Lambert averages 0.5 over a sphere, so 2x it
        // averages 1.0; lerping halfway from flat keeps the mean at 1.0 while giving
        // each cube face a distinct facet. A plain N.L would render the tier at about a
        // quarter of the brightness of the mesh standing next to it.
        float hl = 0.5 + 0.5 * dot(N, normalize(-L.sun_dir.xyz));
        sunPart *= sh * mix(1.0, hl * 2.0, 0.5);
    }

    // Ambient — tree.frag's chain, minus the two screen-space factors it multiplies in
    // (GTAO's coloredAO and the SSIL bounce): both read prepass depth, which these
    // crowns are not in. The AO is not simply lost — the per-voxel colour was baked
    // WITH ambient occlusion and height grading in it.
    // ⚠ The 0.75 crown:grass ambient ratio scales the FIELD too, exactly as tree.frag
    // spells out: the swap must not move the exposure, only the distribution.
    // ⚠ Sampled UP, like every ambient term on foliage — the field lookup is per CUBE,
    // so a per-face normal is not available to it anyway, and matching the crown here
    // is what keeps the two representations the same brightness at the handover.
    const float kTreeAmb = 0.75;
    vec3  skyFill = skyAmbientUp() * (hemi * L.sky_params.y);
    vec4  field   = ddgiFieldWeighted(centre, vec3(0.0, 1.0, 0.0), normalize(toEye));
    // ddgiAmbientFromField without its gather blend: the gather is a screen-space GI
    // buffer resolved at prepass depth, so for this tier it is wrong for the same
    // reason the shadow mask is. Dropping it is a correction, not a shortcut.
    vec3  gi = field.rgb * mix(1.0, hemi, L.ddgi_params.y);
    vec3  ambFill = mix(skyFill, gi, field.w) * kTreeAmb;
    float flatShare = L.ddgi_params.w / max(L.ddgi_params.x, 1e-4);
    vec3  ambient = ambFill + L.ambient.rgb * (1.0 - flatShare * field.w);

    vec3 col = unpackUnorm4x8(V.c).rgb * (ambient + sunPart);

    // Distance fog (R4) — identical to the crown's, or the treeline splits in two.
    float camDist = length(L.eye_pos.xyz - centre);
    float fog = clamp(camDist * L.fog_params.w + L.fog_params.x, 0.0, 1.0);
    vColor = mix(col, L.fog_color.rgb, fog);
}
