// xrRenderVulkan — VSM grass-caster page rasterization BODY (depth-only). Included by
// the two thin stage wrappers (same split the tree hybrid uses):
//   vsm_grass_page.vert    — DYNAMIC atlas (64x32 grid), live SSFX wind. L0 pairs.
//   vsm_grass_page_s.vert  — STATIC atlas (64x96 grid), RIGID (no wind: the page is
//                            CACHED — animated geometry would freeze mid-sway and the
//                            cache would never be reusable). L1/L2 dirty-page pairs.
// Wrapper contract: #define GP_ATLAS_W / GP_ATLAS_H (page grid) and GP_WIND (0/1)
// before including this file.
//
// Draws exactly pairCount[type] instances per type (indirect, written by
// vsm_grass_bin): each gl_InstanceIndex is one COMPACT (instance, page-slot) pair —
// the instance's transform rows are PULLED from the detail VisibleSSBO by the pair's
// local index (no instance-rate vertex attributes; a pair list can't ride
// firstInstance).

layout(location = 0) in vec3  aPos;        // binding 0: grass mesh vertex (pos @0)
layout(location = 1) in vec2  aUV;         // binding 0: uv @12 (for the alpha-test cutout)
layout(location = 2) in float aHeight;     // binding 0: height @20 (wind stiffness)

layout(location = 0) out vec2 vUV;

struct DetailInstance { vec4 row0, row1, row2, color; };   // 64 B (matches DetailInstance)
layout(set = 0, binding = 0) readonly buffer GrassPairs { uint pairs[]; };   // slot(13) << 19 | instLocal(19)
layout(set = 0, binding = 1) readonly buffer PageList  { uvec4 pageList[]; };
#define VSM_PARAMS_SET     0
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)
layout(set = 0, binding = 3) readonly buffer Visible { DetailInstance visInst[]; };

layout(push_constant) uniform PC {
    vec4 wind_params;    // (wind_direction, wind_velocity, treeAmplitude, PER-TYPE wind scale)
    vec4 wsetup_grass;   // SSFX (animspeed, turbulence, push, wave)
    vec4 wind_anim;      // Environment.wind_anim drift (xy) + w = minWindSpeed
    uint instanceBase;   // type * sectionSize (VisibleSSBO section)
    uint pairBase;       // type * pairSection (pair arena section)
} pc;

#if GP_WIND
#define SSFX_WIND_SET 1     // s_waves lives in the detail per-type set (set 1, binding 1)
#include "ssfx_wind.glsl"
#endif

out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

#define VSM_ROUTE_WP       wp
#define VSM_ROUTE_ATLAS_W  GP_ATLAS_W
#define VSM_ROUTE_ATLAS_H  GP_ATLAS_H

void main()
{
    vUV = aUV;   // for the fragment alpha test (blade cutout, not the solid quad)
    uint pair  = pairs[pc.pairBase + uint(gl_InstanceIndex)];
    uint slot  = pair >> 19;                       // < grid size by construction (bin appends resident slots only)
    uint local = pair & 0x7FFFFu;
    DetailInstance di = visInst[pc.instanceBase + local];
    mat4x3 m = mat4x3(di.row0.xyz, di.row1.xyz, di.row2.xyz, vec3(di.row0.w, di.row1.w, di.row2.w));
    vec3 wp = m * vec4(aPos, 1.0);
#if GP_WIND
    // Same SSFX wind the colour pass applies (detail.vert) so the cast shadow
    // tracks the swaying blade; wind_params.w = per-type DO_NO_WAVING scale.
    WindSetup W = ssfx_wind_setup(pc.wind_params, pc.wsetup_grass, pc.wind_anim.w);
    wp += ssfx_wind_grass(wp, aHeight, W, pc.wind_anim.xy) * pc.wind_params.w;
#endif

    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}
