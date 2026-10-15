#include "stdafx.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "IGame_ObjectPool.h"
#include "xr_object.h"
#include "../xr_3da/x_ray.h"

IGame_ObjectPool::IGame_ObjectPool(void) {}

IGame_ObjectPool::~IGame_ObjectPool(void) { R_ASSERT(m_PrefetchObjects.empty()); }

void IGame_ObjectPool::prefetch()
{
    // в огср замороченный префетч объектов (не моделей), требует много переделок в xrGame, потому пусть работает только в тч-режиме.
    if (CApplication::CheckCsCopMode())
        return;

    R_ASSERT(m_PrefetchObjects.empty());

    int p_count = 0;
    ::Render->model_Logging(FALSE);

    string256 section;
    // prefetch objects
    strconcat(sizeof(section), section, "prefetch_objects_", g_pGamePersistent->m_game_params.m_game_type);
    if (pSettings->section_exist(section))
    {
        const auto& sect = pSettings->r_section(section);
        CTimer T;
        T.Start();
        Render->models_begin_prefetch1(true);
        for (const auto& item : sect.Ordered_Data)
        {
            if (pSettings->section_exist(item.first.c_str()))
            {
                CLASS_ID CLS = pSettings->r_clsid(item.first.c_str(), "class");
                p_count++;
                CObject* pObject = (CObject*)NEW_INSTANCE(CLS);
                pObject->Load(item.first.c_str());
                pObject->reload(item.first.c_str());
                VERIFY2(pObject->cNameSect().c_str(), item.first.c_str());
                m_PrefetchObjects.push_back(pObject);
            }
            else
                Msg("! [%s] unknown section %s in %s", __FUNCTION__, item.first.c_str(), section);
        }
        Render->models_begin_prefetch1(false);
        Msg("[%s] objects prefetching time (%zi): [%.2f s.]", __FUNCTION__, p_count, T.GetElapsed_sec());
    }

    // out statistic
    ::Render->model_Logging(TRUE);
}

void IGame_ObjectPool::clear()
{
    // Clear POOL
    ObjectVecIt it = m_PrefetchObjects.begin();
    ObjectVecIt itE = m_PrefetchObjects.end();
    for (; it != itE; it++) {
        (*it)->cNameVisual_set(0);
        xr_delete(*it);
    }
    m_PrefetchObjects.clear();
}

// Spawn-burst profiling (see SpawnProf in xrGameLevel_network_spawn.cpp). This
// call is 1161 deep on the first precache frame and measured 80-111 ms of it.
namespace SpawnProf
{
extern u64 s_clsid, s_alloc, s_objLoad;
struct scope
{
    u64& sink;
    u64  t0;
    scope(u64& s) : sink(s), t0(CPU::QPC()) {}
    ~scope() { sink += CPU::QPC() - t0; }
};
}   // namespace SpawnProf

CObject* IGame_ObjectPool::create(LPCSTR name)
{
    CLASS_ID CLS;
    { SpawnProf::scope _p(SpawnProf::s_clsid);   CLS = pSettings->r_clsid(name, "class"); }
    CObject* O;
    { SpawnProf::scope _p(SpawnProf::s_alloc);   O = (CObject*)NEW_INSTANCE(CLS); }
    { SpawnProf::scope _p(SpawnProf::s_objLoad); O->Load(name); }
    return O;
}

void IGame_ObjectPool::destroy(CObject* O) { xr_delete(O); }
