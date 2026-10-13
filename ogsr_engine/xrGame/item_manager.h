////////////////////////////////////////////////////////////////////////////
//	Module 		: item_manager.h
//	Created 	: 27.12.2003
//  Modified 	: 27.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Item manager
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "object_manager.h"
#include "GameObject.h"

class CCustomMonster;

class CItemManager : public CObjectManager<const CGameObject>
{
public:
    typedef CObjectManager<const CGameObject> inherited;
    typedef OBJECTS ITEMS;

private:
    CCustomMonster* m_object;

    // -----------------------------------------------------------------------
    // ⭐КЭШ ОТВЕТА "ПРЕДМЕТ В МОЕЙ РАЗРЕШЁННОЙ ЗОНЕ" (квар mem_item_access_cache).
    //
    // 📏Замер 19-09-2026 (Кордон/день, 60 fps, прибор npc_stats 1):
    //     stalker/memory_update      0.162 мс/кадр   98 мкс на апдейт сталкера
    //       mem/scan_objects         0.146
    //         scan/item_add          0.122            21.9 мкс на вызов
    //           useful/access_pos    0.070            12.2 мкс
    //           useful/access_vtx    0.052             9.0 мкс
    //           всё остальное        0.001
    // То есть ВЕСЬ CItemManager::useful() -- это два теста на пространственные
    // ограничители, и это самая крупная C++-статья всей фазы планировщика.
    // Цена внутри: accessible(vertex) делает ПЯТЬ тестов сферой (четыре угла
    // клетки и центр), accessible(position) -- до пяти проверок inside/on_border.
    //
    // Ответ зависит ТОЛЬКО от позиции предмета и от набора ограничителей NPC.
    // Предмет на земле не двигается вообще, поэтому запись годна, пока совпадают
    // ВСЕ три ключа: вершина ИИ, позиция и ПОКОЛЕНИЕ ограничителей.
    // ⚠Поколение берётся у CSpaceRestrictionManager, а НЕ через on_restrictions_change():
    // add_border/remove_border меняют геометрию и этот хук НЕ зовут.
    // ⚠Дефолт ВЫКЛ: это кэш решения ИИ, и включать его надо после живой проверки
    // того, что сталкеры по-прежнему подбирают вещи.
    // -----------------------------------------------------------------------
    struct SAccess
    {
        u32 vertex_id;
        Fvector position;
        u32 epoch;
        bool ok;
    };
    mutable xr_map<u16, SAccess> m_access_cache;

    bool accessible_cached(const CGameObject* object) const;

public:
    IC CItemManager(CCustomMonster* object);
    virtual bool useful(const CGameObject* object) const;
    virtual bool is_useful(const CGameObject* object) const;
    virtual float evaluate(const CGameObject* object) const;
    virtual float do_evaluate(const CGameObject* object) const;
    virtual void update();
    void remove_links(CObject* object);
    virtual void on_restrictions_change();
};

#include "item_manager_inline.h"