// xrRenderVulkan — the bone palette every skinned VERTEX pass indexes as `bones[]`
// (see skin_matrix.glsl). Two ways to the same matrices:
//
//   default          binding 0, the whole bone SSBO; pc.baseBone is an absolute slot.
//   SKIN_BONES_UBO   binding 1, a 1024-bone WINDOW of the same buffer as a dynamic UBO
//                    (r_skin_ubo, the *_ubo.vert.spv twins). The C++ side binds the
//                    window at a 512-bone-aligned dynamic offset and pushes baseBone
//                    RELATIVE to it (vk_pass_skinned.cpp, kBoneWin). Why: on pre-Turing
//                    NVK (the Switch) an SSBO read is a memory load, a dynamic UBO is the
//                    constant bank — in every stage.
//
// kBoneWin in vk_pass_skinned.cpp MUST equal the array size below.
#ifndef SKIN_BONES_GLSL
#define SKIN_BONES_GLSL

#ifdef SKIN_BONES_UBO
layout(std140, set = 0, binding = 1) uniform BonesWin { mat4 bones[1024]; };
#else
layout(std430, set = 0, binding = 0) readonly buffer Bones { mat4 bones[]; };
#endif

#endif // SKIN_BONES_GLSL
