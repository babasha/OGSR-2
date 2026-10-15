////////////////////////////////////////////////////////////////////////////
//	Module 		: trade_parameters.cpp
//	Created 	: 13.01.2006
//  Modified 	: 13.01.2006
//	Author		: Dmitriy Iassenev
//	Description : trade parameters class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "trade_parameters.h"

CTradeParameters* CTradeParameters::g_default_instance = 0;

std::shared_ptr<const CTradeFactorParameters::Data>* trade_section_cache(LPCSTR ini_fname, const shared_str& section)
{
    if (!ini_fname || !ini_fname[0] || !section.size())
        return nullptr;
    // a handful of trade configs x their sections for the whole session; the main thread and the spawn thread both get
    // here, but only from Lua, which never runs on both at once
    static xr_unordered_map<xr_string, std::shared_ptr<const CTradeFactorParameters::Data>> s_cache;
    xr_string key(ini_fname);
    key += '|';
    key += section.c_str();
    return &s_cache[key];
}

void CTradeParameters::process(action_show, CInifile& ini_file, const shared_str& section)
{
    VERIFY(ini_file.section_exist(section));
    m_show.clear();
    CInifile::Sect& S = ini_file.r_section(section);
    for (const auto& I : S.Data)
        if (!I.second.size())
            m_show.disable(I.first);
}
