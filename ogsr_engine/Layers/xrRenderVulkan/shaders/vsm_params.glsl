// xrRenderVulkan — VSM clipmap parameters (light view, per-level rect, depth range).
//
// The block itself is the same everywhere; only WHERE it is bound changes, because
// each pass lays its descriptor sets out differently (set 2 b2 for the tree casters,
// set 0 b1 for the binning computes, set 0 b2 for grass/static pages, set 1 b2 for
// the skinned page, set 0 b8 for the voxel cull). So it was written out SEVENTEEN
// times — identical bodies, five different layout qualifiers — and had to be edited
// in all seventeen whenever the clipmap gained a field.
//
// Define VSM_PARAMS_SET / VSM_PARAMS_BINDING before including to place it; the
// defaults match the binning computes. Same idea as ENV_SET in light_ubo.glsl.
//
// #include AFTER vsm_common.glsl (needs VSM_LEVELS).
#ifndef VSM_PARAMS_GLSL
#define VSM_PARAMS_GLSL

#ifndef VSM_PARAMS_SET
#define VSM_PARAMS_SET 0
#endif
#ifndef VSM_PARAMS_BINDING
#define VSM_PARAMS_BINDING 1
#endif

layout(set = VSM_PARAMS_SET, binding = VSM_PARAMS_BINDING) uniform VsmParams {
    mat4 view;                  // world -> light space
    vec4 level[VSM_LEVELS];     // per clipmap level: xy = origin, z = world extent
    vec4 zparams;               // x = z origin, y = 1/range (world -> [0,1] depth)
} vsm;

#endif // VSM_PARAMS_GLSL
