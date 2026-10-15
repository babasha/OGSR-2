#pragma once

class IUIShader;

class IUIRender
{
public:
    enum ePrimitiveType
    {
        ptNone = -1,
        ptTriList,
        ptTriStrip,
        ptLineStrip,
        ptLineList
    };

    enum ePointType
    {
        pttNone = -1,
        pttTL,
        pttLIT
    };

    enum CullMode
    {
        cmNONE = 0,
        cmCW,
        cmCCW,
    };

public:
    // virtual ~IUIRender() {;}

    virtual void CreateUIGeom() = 0;
    virtual void DestroyUIGeom() = 0;

    virtual void SetShader(IUIShader& shader) = 0;
    virtual void SetAlphaRef(int aref) = 0;

    virtual void SetScissor(Irect* rect = nullptr) = 0;
    virtual void GetActiveTextureResolution(Fvector2& res) = 0;

    virtual void PushPoint(float x, float y, float z, u32 C, float u, float v) = 0;

    virtual void StartPrimitive(u32 iMaxVerts, ePrimitiveType primType, ePointType pointType) = 0;
    virtual void FlushPrimitive() = 0;

    virtual LPCSTR UpdateShaderName(LPCSTR tex_name, LPCSTR sh_name) = 0;

    virtual void CacheSetXformWorld(const Fmatrix& M) = 0;
    virtual void CacheSetCullMode(CullMode) = 0;

    // ⭐Textures the caller already holds as pixels, with no file behind them.
    //
    // SetShader is the only way to bind an image here, and IUIShader::create
    // takes NAMES -- which is enough for the XML UI, whose every image is a file,
    // and not enough for anything that builds its own atlas. A font engine is the
    // obvious case: RmlUi rasterises glyphs with FreeType and hands the renderer
    // a block of RGBA it just generated. Without this there is no way to show it.
    //
    // Defaulted rather than pure so a renderer that cannot do this stays valid
    // and simply answers "no" -- the caller checks the handle.
    virtual void* DynTextureCreate(const void* /*rgba*/, u32 /*w*/, u32 /*h*/) { return nullptr; }
    virtual void DynTextureDestroy(void* /*handle*/) {}
    // Binds a dynamic texture for the next FlushPrimitive, where SetShader would
    // otherwise be called. Passing nullptr means "untextured" (plain white).
    virtual void SetDynTexture(void* /*handle*/) {}
};