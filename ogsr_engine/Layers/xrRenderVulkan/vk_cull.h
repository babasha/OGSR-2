// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

#pragma once

// Shared frustum-plane extraction (Gribb/Hartmann, D3D-style row-major view-proj).
// Was copy-pasted in vk_DetailManager_Render.cpp, vk_TreeManager_Render.cpp (and
// mirrored in tree_cull.comp.glsl). Plane order: L,R,B,T,N,F. Each plane is
// normalized so .xyz is a unit normal and .w the signed distance, giving
// dot(plane.xyz, p) + plane.w as the point-to-plane distance for culling.
// Relies on the includer pulling xrCore math (Fmatrix/Fvector4) first.
namespace VK {

inline void ExtractFrustumPlanes(const Fmatrix& m, Fvector4 planes[6])
{
    planes[0].set(m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41); // left
    planes[1].set(m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41); // right
    planes[2].set(m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42); // bottom
    planes[3].set(m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42); // top
    planes[4].set(m._14 + m._13, m._24 + m._23, m._34 + m._33, m._44 + m._43); // near
    planes[5].set(m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43); // far
    for (int i = 0; i < 6; ++i) {
        const float L = _sqrt(planes[i].x * planes[i].x + planes[i].y * planes[i].y + planes[i].z * planes[i].z);
        if (L > 0.0001f) {
            const float inv = 1.0f / L;
            planes[i].x *= inv; planes[i].y *= inv; planes[i].z *= inv; planes[i].w *= inv;
        }
    }
}

}  // namespace VK
