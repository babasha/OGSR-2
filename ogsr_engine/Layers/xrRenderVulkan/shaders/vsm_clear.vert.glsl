#version 450
#extension GL_GOOGLE_include_directive : require
// xrRenderVulkan — VSM dirty-page clear (Phase 1b). Depth-only, instanced: one instance per
// dirty static slot draws a 2-triangle quad covering that slot's atlas cell at depth 1.0 (far
// = no occluder). Runs first in the static atlas pass (loadOp LOAD), so only dirty pages are
// reset; cached pages keep their depth. The caster draws then write the nearest occluder
// (LESS_OR_EQUAL) over the cleared cells. instanceCount = dirty count (indirect). See vk_vsm.cpp.
#include "vsm_common.glsl"

layout(set = 0, binding = 0) readonly buffer DirtyList { uint dirtyList[]; };

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    uint slot = dirtyList[gl_InstanceIndex];
    uint ax = slot % uint(VSM_ATLAS_W_S);
    uint ay = slot / uint(VSM_ATLAS_W_S);

    // 2-triangle quad corners: tri1 (0,0)(1,0)(1,1)  tri2 (0,0)(1,1)(0,1).
    int  i = gl_VertexIndex;
    vec2 corner = (i == 0) ? vec2(0.0, 0.0) :
                  (i == 1) ? vec2(1.0, 0.0) :
                  (i == 2) ? vec2(1.0, 1.0) :
                  (i == 3) ? vec2(0.0, 0.0) :
                  (i == 4) ? vec2(1.0, 1.0) : vec2(0.0, 1.0);

    vec2 cellMin  = vec2(float(ax), float(ay)) / vec2(float(VSM_ATLAS_W_S), float(VSM_ATLAS_H_S));
    vec2 cellSize = 1.0 / vec2(float(VSM_ATLAS_W_S), float(VSM_ATLAS_H_S));
    vec2 uv = cellMin + corner * cellSize;
    gl_Position = vec4(uv * 2.0 - 1.0, 1.0, 1.0);   // atlas NDC, depth 1.0
}
