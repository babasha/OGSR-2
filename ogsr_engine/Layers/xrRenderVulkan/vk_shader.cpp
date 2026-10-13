// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// ============================================================================
// vk_shader.cpp - Vulkan Shader System Implementation
// ============================================================================
//
// Phase 2.32: Shader System Implementation
//
// Implements shader loading and pipeline mapping for Vulkan renderer.
//
// ============================================================================

#include "stdafx.h"
#include "vk_shader.h"
#include "vk_material.h"
#include <unordered_set>   // one-shot "mentions water but isn't ours" diagnostic
#include <string>

// Global instance
VK::CVulkanShaderManager* g_VulkanShaderManager = nullptr;

namespace VK
{

// ============================================================================
// CVulkanShader Implementation
// ============================================================================

CVulkanShader::CVulkanShader()
    : m_Material(nullptr)
    , m_bEmissive(false)
    , m_bDistort(false)
    , m_bLandscape(false)
    , m_bWmark(false)
    , m_bAlphaRef(false)
    , m_bGlass(false)
    , m_bWater(false)
{
}

CVulkanShader::~CVulkanShader()
{
    Destroy();
}

void CVulkanShader::Create(LPCSTR name, LPCSTR tex_diffuse)
{
    m_Name = name;

    // Texture list from level shaders is comma-separated, format varies:
    //   "diffuse"                     — solid material, no lightmap (vert-lit)
    //   "diffuse,bump"                — bumped material
    //   "diffuse,bump,lmap#####"      — lightmapped (lmap path)
    //   "diffuse,lmap1,lmap2"         — legacy 2-texture lightmap (R1/R2)
    // Slot 2 (third comma-separated entry) is what `uber_deffer.cpp` resolves
    // as the lightmap when its name starts with "lmap". We keep it verbatim
    // here; WorldMaterialCache decides whether to bind it.
    if (tex_diffuse && tex_diffuse[0])
    {
        string256 buf;
        xr_strcpy(buf, tex_diffuse);

        // Tokenize by comma in-place. `slots[i]` points into `buf` after
        // each comma is replaced by NUL; safe because m_TexDiffuse / m_TexLmap
        // copy into shared_str on assignment.
        LPCSTR slots[3] = { nullptr, nullptr, nullptr };
        u32    slot_idx = 0;
        slots[0] = buf;
        for (LPSTR p = buf; *p && slot_idx < 2; ++p) {
            if (*p == ',') { *p = 0; ++slot_idx; slots[slot_idx] = p + 1; }
        }

        if (slots[0] && slots[0][0]) m_TexDiffuse = slots[0];
        if (slots[2] && slots[2][0]) m_TexLmap    = slots[2];
    }
    else
    {
        m_TexDiffuse = tex_diffuse;
    }

    // Parse shader name for flags
    // Example: "def_shaders\def_aref" - alpha reference (alpha test)
    // Example: "def_shaders\lod_def" - LOD shader
    xr_string shader_lower = name;
    std::transform(shader_lower.begin(), shader_lower.end(), shader_lower.begin(), ::tolower);

    // Check for alpha test (aref)
    if (shader_lower.find("aref") != xr_string::npos ||
        shader_lower.find("alpha") != xr_string::npos ||
        shader_lower.find("trans") != xr_string::npos) {
        // Alpha test shaders use discard in fragment shader (alphaRef > 0)
        m_bAlphaRef = true;
    }

    // Translucent GLASS: the engine "glass" shader (model/lamp panes), or a
    // trans/aref LEVEL shader whose diffuse lives in glas\ / wnd (windows,
    // e.g. def_trans + glas\glas_dirt on Cordon). R4 draws these BLENDED; our
    // opaque path alpha-TESTED them at 0.78 — a semi-transparent pane has no
    // texel that passes, so the glass was simply invisible. Grates/fences on
    // def_trans keep the alpha test (their textures aren't under glas\/wnd).
    {
        xr_string tex_lower = m_TexDiffuse.size() ? m_TexDiffuse.c_str() : "";
        std::transform(tex_lower.begin(), tex_lower.end(), tex_lower.begin(), ::tolower);
        // glas\ textures are glass BY DEFINITION — flag them regardless of the
        // shader (the village windows use plain `default` + glas\glas_windows1
        // and stayed opaque under the aref-only rule). "wnd" alone is too loose
        // (window FRAMES live there too) — it still requires a trans shader.
        if (shader_lower.find("glass") != xr_string::npos ||
            tex_lower.find("glas\\") != xr_string::npos ||
            tex_lower.rfind("glas", 0) == 0 ||
            (m_bAlphaRef && tex_lower.find("wnd") != xr_string::npos))
            m_bGlass = true;
    }

    // WATER BODIES. Level water is `effects\water` (+ `effects\water_clear` and
    // friends) — ordinary static geometry that, unflagged, rode the opaque
    // vert-lit path and rendered BLACK everywhere: a water polygon carries no
    // baked vertex light and no baked sky access, so albedo × light == 0. Give
    // it its own class and Pass_Water draws it blended (vk_pass_water.cpp).
    //
    // Matched by PREFIX, not by substring: `models\water` is an ARTIFACT model
    // shader (artefact_rusty_hairs), and routing a mesh prop into a horizontal
    // water surface pass would be a spectacular way to lose an artifact.
    if (shader_lower.rfind("effects\\water", 0) == 0 || shader_lower.rfind("effects/water", 0) == 0) {
        m_bWater = true;
    } else if (shader_lower.find("water") != xr_string::npos) {
        // Everything else that merely mentions water — logged once per name so
        // non-stock content using a different water shader is VISIBLE here
        // rather than silently staying black.
        static std::unordered_set<std::string> s_seenWater;
        if (s_seenWater.size() < 64 && s_seenWater.insert(shader_lower.c_str()).second)
            Msg("[VK Water] shader '%s' mentions water but is NOT `effects\\water*` -> stays on the ordinary path", name);
    }

    // Check for additive shaders (effects, glows)
    if (shader_lower.find("add") != xr_string::npos ||
        shader_lower.find("glow") != xr_string::npos) {
        m_bEmissive = true;
    }

    // Check for distortion
    if (shader_lower.find("dist") != xr_string::npos) {
        m_bDistort = true;
    }

    // Check for landscape
    if (shader_lower.find("terrain") != xr_string::npos ||
        shader_lower.find("landscape") != xr_string::npos) {
        m_bLandscape = true;
    }

    // Check for wallmarks (decals). NOTE "wallmark" too — baked level decals
    // (newspapers, dirt overlays) use shader "effects\wallmark*", and "wmark"
    // is NOT a substring of "wallmark" (the original check silently missed
    // every level decal → they rendered opaque + coplanar → z-fight flicker).
    if (shader_lower.find("wallmark") != xr_string::npos ||
        shader_lower.find("wmark") != xr_string::npos ||
        shader_lower.find("decal") != xr_string::npos) {
        m_bWmark = true;
    }

    Msg("[Vulkan] Shader created: %s (diffuse: %s)", name, tex_diffuse);
}

void CVulkanShader::Destroy()
{
    // Don't destroy material here - it's managed by MaterialManager
    m_Material = nullptr;
}

VK::CMaterial* CVulkanShader::GetMaterial()
{
    // Lazy material creation
    if (!m_Material && m_TexDiffuse.size() > 0) {
        if (g_MaterialManager) {
            // Use texture name as material name
            m_Material = g_MaterialManager->CreateMaterial(m_TexDiffuse.c_str());
        }
    }

    return m_Material;
}

// ============================================================================
// CVulkanShaderManager Implementation
// ============================================================================

CVulkanShaderManager::CVulkanShaderManager()
    : m_DefaultShader(nullptr)
    , m_bCreated(false)
{
}

CVulkanShaderManager::~CVulkanShaderManager()
{
    Destroy();
}

void CVulkanShaderManager::Create()
{
    if (m_bCreated) return;

    Msg("[Vulkan] Creating Vulkan Shader Manager...");

    // Create default shader (white material)
    m_DefaultShader = xr_new<CVulkanShader>();
    m_DefaultShader->Create("default", "");  // Empty texture = white fallback

    m_bCreated = true;
    Msg("[Vulkan] Vulkan Shader Manager created successfully");
}

void CVulkanShaderManager::Destroy()
{
    if (!m_bCreated) return;

    Msg("[Vulkan] Destroying Vulkan Shader Manager...");

    // Destroy default shader
    if (m_DefaultShader) {
        m_DefaultShader->Destroy();
        xr_delete(m_DefaultShader);
    }

    // Destroy all shaders
    for (auto& pair : m_Shaders) {
        if (pair.second) {
            pair.second->Destroy();
            xr_delete(pair.second);
        }
    }
    m_Shaders.clear();

    m_bCreated = false;
    Msg("[Vulkan] Vulkan Shader Manager destroyed");
}

CVulkanShader* CVulkanShaderManager::CreateShader(LPCSTR name, LPCSTR tex_diffuse)
{
    if (!name || !name[0]) {
        Msg("![Vulkan] Cannot create shader - empty name");
        return m_DefaultShader;
    }

    // Build unique key from shader name + texture name
    // (same shader can be used with different textures in the level shader table)
    string512 key;
    xr_sprintf(key, "%s#%s", name, tex_diffuse ? tex_diffuse : "");

    // Check if shader already exists
    auto it = m_Shaders.find(key);
    if (it != m_Shaders.end()) {
        return it->second;
    }

    // Create new shader
    CVulkanShader* shader = xr_new<CVulkanShader>();
    shader->Create(name, tex_diffuse ? tex_diffuse : "");

    // Add to cache
    m_Shaders[key] = shader;

    return shader;
}

CVulkanShader* CVulkanShaderManager::GetShader(LPCSTR name)
{
    if (!name || !name[0]) {
        return m_DefaultShader;
    }

    // Check cache
    auto it = m_Shaders.find(name);
    if (it != m_Shaders.end()) {
        return it->second;
    }

    // Return default shader if not found
    Msg("![Vulkan] Shader not found: %s, using default", name);
    return m_DefaultShader;
}

void CVulkanShaderManager::DestroyShader(CVulkanShader* shader)
{
    if (!shader) return;

    // Remove from cache
    for (auto it = m_Shaders.begin(); it != m_Shaders.end(); ++it) {
        if (it->second == shader) {
            shader->Destroy();
            xr_delete(shader);
            m_Shaders.erase(it);
            return;
        }
    }
}

} // namespace VK
