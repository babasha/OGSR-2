// Rain.h: interface for the CRain class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

// refs
class ENGINE_API IRender_DetailModel;
class ENGINE_API CLAItem;

#include "../Include/xrRender/FactoryPtr.h"
#include "../Include/xrRender/LensFlareRender.h"
#include "../Include/xrRender/ThunderboltDescRender.h"
#include "../Include/xrRender/ThunderboltRender.h"

class CEnvironment;

struct SThunderboltDesc
{
    // geom
    // IRender_DetailModel*		l_model;
    FactoryPtr<IThunderboltDescRender> m_pRender;
    // sound
    ref_sound snd;
    // gradient
    struct SFlare
    {
        float fOpacity;
        Fvector2 fRadius;
        shared_str texture;
        shared_str shader;
        // ref_shader				hShader;
        FactoryPtr<IFlareRender> m_pFlare;
        SFlare()
        {
            fOpacity = 0;
            fRadius.set(0.f, 0.f);
        }
    };
    SFlare* m_GradientTop;
    SFlare* m_GradientCenter;
    shared_str name;
    CLAItem* color_anim{};

public:
    SThunderboltDesc();
    ~SThunderboltDesc();

    void load(CInifile& pIni, shared_str const& sect);
    void create_top_gradient(CInifile& pIni, shared_str const& sect);
    void create_center_gradient(CInifile& pIni, shared_str const& sect);

    void load_shoc(CInifile* pIni, shared_str const& sect);
    void create_top_gradient_shoc(CInifile* pIni, shared_str const& sect);
    void create_center_gradient_shoc(CInifile* pIni, shared_str const& sect);
};

struct SThunderboltCollection
{
    DEFINE_VECTOR(SThunderboltDesc*, DescVec, DescIt);
    DescVec palette;
    shared_str section;

public:
    SThunderboltCollection();
    ~SThunderboltCollection();
    void load(CInifile* pIni, CInifile* thunderbolts, LPCSTR sect);
    void load_shoc(CInifile* pIni, LPCSTR sect);
    SThunderboltDesc* GetRandomDesc() { return palette.at(Random.randI(palette.size())); }
};

#define THUNDERBOLT_CACHE_SIZE 8
//
class ENGINE_API CEffect_Thunderbolt
{
    friend class vkThunderboltRender;

protected:
    DEFINE_VECTOR(SThunderboltCollection*, CollectionVec, CollectionVecIt);
    CollectionVec collection;
    SThunderboltDesc* current;

private:
    Fmatrix current_xform;
    Fvector3 current_direction;

    FactoryPtr<IThunderboltRender> m_pRender;
    // ref_geom			  		hGeom_model;
    //  states
    enum EState
    {
        stIdle,
        stWorking
    };
    EState state;

    // ref_geom			  		hGeom_gradient;

    Fvector lightning_center;
    float lightning_size{};
    float lightning_phase{};
    // The colour this bolt is currently ADDING to the env (the animated flash, 0
    // when idle). Published so the renderer can light the sky with it instead of
    // following the fake sun the bolt installs in sun_dir — see Flash().
    Fvector current_flash{};

    float life_time;
    float current_time{};
    float next_lightning_time;
    BOOL bEnabled;

private:
    BOOL RayPick(const Fvector& s, const Fvector& d, float& range);
    void Bolt(shared_str id, float period, float life_time);

public:
    CEffect_Thunderbolt();
    ~CEffect_Thunderbolt();

    void OnFrame(shared_str id, float period, float duration);
    void Render(CBackend& cmd_list);

    // True while a bolt is flashing. During this window OnFrame() overwrites
    // CurrentEnv->sun_dir with the bolt direction, so shadow code must hold its
    // last real sun direction instead of following it (see vk_pass_shadow).
    bool IsActive() const { return state == stWorking; }

    // Flash colour being added this frame (0,0,0 when no bolt). A real lightning
    // strike is a huge, very distant area light: it lifts the whole sky, it does
    // not put a second sun in it. The engine's own trick does the opposite —
    // sun_dir is swung at the strike, so everything that draws AT the sun (sky
    // disc, volumetric sun beam) sprouts a duplicate sun with god rays fanning out
    // of it, one per bolt in the clap. The renderer holds the real sun direction
    // and spends THIS instead, as sky/ambient light.
    const Fvector& Flash() const { return current_flash; }

    shared_str AppendDef(CEnvironment& environment, CInifile* pIni, CInifile* thunderbolts, LPCSTR sect);
    shared_str AppendDef_shoc(CEnvironment& environment, CInifile* pIni, LPCSTR sect);
};
