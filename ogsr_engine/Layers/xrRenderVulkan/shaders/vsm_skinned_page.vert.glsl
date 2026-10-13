#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM skinned-caster page rasterization (depth-only). Combines the
// GPU skinning of shadow_skinned.vert (bones pre-multiplied by the object world matrix
// → S*pos is world space) with the page routing of vsm_page.vert (each instance = one
// allocated atlas page the leaf overlaps: gl_InstanceIndex → casterPages[] → slot →
// pageList[] → page ortho + atlas sub-rect, gl_ClipDistance clips to the page). So NPCs
// cast into the same virtual shadow atlas the statics do. See vk_vsm.cpp.
#include "vsm_common.glsl"

// vertHW skinned attributes — MUST match BuildSkinnedVI (skinned.vert / shadow_skinned.vert).
layout(location = 0) in vec4 a_Position;
layout(location = 1) in vec4 a_Normal;
layout(location = 2) in vec4 a_TexCoordExt;
layout(location = 3) in vec4 a_Tangent;
layout(location = 4) in vec4 a_Binormal;
layout(location = 5) in vec4 a_BoneIndices;

layout(std430, set = 0, binding = 0) readonly buffer Bones { mat4 bones[]; };   // shared skinned bone SSBO (world-space)

layout(set = 1, binding = 0) readonly buffer PageList    { uvec4 pageList[]; };   // slot -> (level, px, py, _)
layout(set = 1, binding = 1) readonly buffer CasterPages { uint  casterPages[]; };// instance -> slot
#define VSM_PARAMS_SET     1
#define VSM_PARAMS_BINDING 2
#include "vsm_params.glsl"   // VsmParams UBO (clipmap view/levels/depth)

layout(push_constant) uniform PC { uint skinMode; uint baseBone; uint boneCount; uint pad; } pc;

out gl_PerVertex { vec4 gl_Position; float gl_ClipDistance[4]; };

#include "skin_matrix.glsl"   // dN/dF/clampB + skinMatrix() — shared by all skinned passes

#define VSM_ROUTE_WP       wp
#define VSM_ROUTE_ATLAS_W  VSM_ATLAS_W
#define VSM_ROUTE_ATLAS_H  VSM_ATLAS_H

void main()
{
    // --- Skin → world position (same math as shadow_skinned.vert).
    vec3 pos = a_Position.xyz;
    mat4 S = skinMatrix();
    vec3 wp = (S * vec4(pos, 1.0)).xyz;

    // --- Route to the atlas page (mirror vsm_page.vert).
    uint slot = casterPages[gl_InstanceIndex];
    if (slot >= uint(VSM_MAX_PHYS)) {            // defensive: cull degenerate instance
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_ClipDistance[0] = gl_ClipDistance[1] = gl_ClipDistance[2] = gl_ClipDistance[3] = -1.0;
        return;
    }
    // Page routing (clip planes + atlas sub-rect) — shared by all VSM casters.
#include "vsm_page_route.glsl"
}
