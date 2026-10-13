////////////////////////////////////////////////////////////////////////////
//	Module 		: xrServer_Factory.cpp
//	Created 	: 19.09.2002
//  Modified 	: 04.06.2003
//	Author		: Oles Shyshkovtsov, Alexander Maksimchuk, Victor Reutskiy and Dmitriy Iassenev
//	Description : Server objects factory
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "object_factory.h"
#include "clsid_game.h"

CSE_Abstract* F_entity_Create(LPCSTR section)
{
    // Compatibility: foreign spawns (Living Zone / era2 edition) reference object sections
    // that don't exist in this config (campfire, custom NPCs, items, smart terrains, ...).
    // Instead of a fatal "Can't open section", substitute an inert graph_point placeholder so
    // the spawn graph loads intact and the actor still drops into the level (чистое рендер-демо).
    // graph_point's STATE_Read reads only bounded fields (no count-driven loops), so parsing the
    // foreign object's bytes can't overrun/OOM. OGSR-native sections are unaffected.
    if (!pSettings->section_exist(section))
    {
        Msg("! [F_entity_Create] unknown section '%s' -> inert space_restrictor placeholder", section);
        return object_factory().server_object(CLSID_SPACE_RESTRICTOR, section);
    }

    return object_factory().server_object(pSettings->r_clsid(section, "class"), section);
}
