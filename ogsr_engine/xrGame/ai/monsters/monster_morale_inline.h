#pragma once

IC void CMonsterMorale::change(float value)
{
    m_morale += value;
    clamp(m_morale, 0.f, 1.f);
}

IC bool CMonsterMorale::is_despondent() { return ((m_state == eDespondent) || (m_morale < m_despondent_threshold)); }

