////////////////////////////////////////////////////////////////////////////
//	Module 		: trade_factor_parameters_inline.h
//	Created 	: 13.01.2006
//  Modified 	: 13.01.2006
//	Author		: Dmitriy Iassenev
//	Description : trade factor parameters class inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CTradeFactorParameters::CTradeFactorParameters() {}

IC CTradeFactorParameters::Data& CTradeFactorParameters::own()
{
    // the table may be the one shared through CTradeParameters::process's cache: write to a copy of our own
    if (!m_data)
        m_data = std::make_shared<Data>();
    else if (m_data.use_count() > 1)
        m_data = std::make_shared<Data>(*m_data);
    return const_cast<Data&>(*m_data);
}

IC void CTradeFactorParameters::clear() { m_data.reset(); }

IC void CTradeFactorParameters::enable(const shared_str& section, const CTradeFactors& factors)
{
    Data& d = own();
    // "/<pattern>/" -- exactly what the regex "^/([^/]+)/$" this replaced matched: '/' first and last, at least one
    // character between, none of them '/'. 📏08-10-2026 Switch: running that std::regex over every line of a trade section
    // (hundreds of items, buy + sell, for every NPC coming online: trade_manager.trade_init) was 3.6-4.5 ms per NPC.
    const char* s = section.c_str();
    const size_t n = s ? xr_strlen(s) : 0;
    if (n >= 3 && s[0] == '/' && s[n - 1] == '/' && !memchr(s + 1, '/', n - 2))
    {
        std::regex re(xr_string(s + 1, n - 2));
        d.m_factors_re.emplace_back(std::move(re), factors);
    }
    else
    {
        if (d.m_factors.find(section) != d.m_factors.end())
            Msg("~~[%s] duplicate of CTradeFactors for section [%s] found!", __FUNCTION__, section.c_str());
        d.m_factors.emplace(section, factors);
    }
}

IC bool CTradeFactorParameters::enabled(const shared_str& section) const
{
    if (!m_data)
        return false;
    auto I = m_data->m_factors.find(section);
    if (I != m_data->m_factors.end())
        return true;
    for (const auto& it : m_data->m_factors_re)
    {
        std::smatch results;
        const xr_string str(section.c_str());
        if (std::regex_search(str, results, it.first))
        {
            return true;
        }
    }
    return false;
}

IC const CTradeFactors& CTradeFactorParameters::factors(const shared_str& section) const
{
    if (m_data)
    {
        auto I = m_data->m_factors.find(section);
        if (I != m_data->m_factors.end())
            return (*I).second;
        for (const auto& it : m_data->m_factors_re)
        {
            std::smatch results;
            const xr_string str(section.c_str());
            if (std::regex_search(str, results, it.first))
            {
                return it.second;
            }
        }
    }
    ASSERT_FMT(false, "[%s]: %s not found", __FUNCTION__, section.c_str());
    static const CTradeFactors s_none;
    return s_none;
}

IC bool CTradeFactorParameters::disabled(const shared_str& section) const { return enabled(section) ? factors(section).disabled() : false; }
