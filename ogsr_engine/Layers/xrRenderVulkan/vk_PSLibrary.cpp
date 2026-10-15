// xrRenderVulkan - compatibility stub for shared X-Ray Engine code.
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha) for the
// stub itself; the interface it mirrors is GSC Game World code (root LICENSE.md).
// Non-commercial use only; keep this notice on redistribution.

// xrRenderVulkan - compat wrapper: compiles OGSR's shared PSLibrary.cpp
// (CPSLibrary — loads all particle definitions from particles.xr). Engine-
// agnostic; needed so model_CreateParticles can FindPED/FindPGD by name.
#include "stdafx.h"

#define FBasicVisualH
#include "vk_FBasicVisual.h"
#include "CRender_Vulkan.h"
#include "vk_ModelPool.h"
#include "../../xrParticles/psystem.h"

#include "../xrRender/PSLibrary.cpp"

// The particle definition library lives here (compat TU) so the PS headers —
// whose base maps dxRender_Visual->vkRender_Visual — never leak into non-compat
// code. Exposed to the rest of the renderer via plain extern fns.
static CPSLibrary g_vkPSLibrary;

CPSLibrary& VK_PSLibrary() { return g_vkPSLibrary; }

void vkParticles_OnCreate()  { g_vkPSLibrary.OnCreate(); }
void vkParticles_OnDestroy() { g_vkPSLibrary.OnDestroy(); }

// Definition lookups consumed by the dedicated vk particle visuals
// (vk_ParticleEffect.cpp / vk_ParticleGroup.cpp build the simulated visual).
PS::CPEDef* VK_FindPED(const char* name) { return (name && name[0]) ? g_vkPSLibrary.FindPED(name) : nullptr; }
PS::CPGDef* VK_FindPGD(const char* name) { return (name && name[0]) ? g_vkPSLibrary.FindPGD(name) : nullptr; }

// Name enumeration + time-limit query for the IRender_interface particle API.
void VK_ParticleEffectFillName(xr_vector<shared_str>& dest)
{
    for (const auto& [name, ped] : g_vkPSLibrary.IteratePEDs())
        dest.push_back(name);
}

void VK_ParticleGroupFillName(xr_vector<shared_str>& dest)
{
    for (const auto& [name, pgd] : g_vkPSLibrary.IteratePGDs())
        dest.push_back(name);
}

float VK_GetParticlesTimeLimit(const char* name)
{
    if (!name || !name[0]) return 0.f;
    if (PS::CPEDef* ped = g_vkPSLibrary.FindPED(name))
        return ped->m_Flags.is(PS::CPEDef::dfTimeLimit) ? ped->m_fTimeLimit : 0.f;
    if (PS::CPGDef* pgd = g_vkPSLibrary.FindPGD(name))
        return pgd->m_fTimeLimit;
    return 0.f;
}

#undef dxRender_Visual
#undef FBasicVisualH
