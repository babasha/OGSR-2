////////////////////////////////////////////////////////////////////////////
//	Module 		: trade_factor_parameters.h
//	Created 	: 13.01.2006
//  Modified 	: 13.01.2006
//	Author		: Dmitriy Iassenev
//	Description : trade factor parameters class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "associative_vector.h"
#include "trade_factors.h"

#include <regex>

class CTradeFactorParameters
{
public:
    struct Data
    {
        string_unordered_map<shared_str, CTradeFactors> m_factors;
        xr_vector<std::pair<std::regex, CTradeFactors>> m_factors_re;
    };

private:
    // Shared between every NPC whose trade section is the same (CTradeParameters::process caches the parsed section);
    // copied before a write. Empty = nothing enabled.
    std::shared_ptr<const Data> m_data;
    IC Data& own();

public:
    IC CTradeFactorParameters();
    IC void clear();
    IC void enable(const shared_str& section, const CTradeFactors& factors);
    IC bool enabled(const shared_str& section) const;
    IC bool disabled(const shared_str& section) const;
    IC const CTradeFactors& factors(const shared_str& section) const;
    IC void share(const std::shared_ptr<const Data>& data) { m_data = data; }
    IC const std::shared_ptr<const Data>& shared() const { return m_data; }
};

#include "trade_factor_parameters_inline.h"
