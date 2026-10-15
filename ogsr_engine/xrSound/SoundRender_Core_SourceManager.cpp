#include "stdafx.h"

#include <chrono>

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"

SndSourceCounters g_snd_sources;

// ⭐11-10-2026: a hash instead of the walk over every source ever created (thousands by mid-game; an NPC's spawn
// asks for ~54 at once, each a strcmp walk), and a lock -- spawns create sounds on worker threads (seqParallel) while
// the level's own loads may still run, and the vector was pushed to unguarded.
CSoundRender_Source* CSoundRender_Core::i_create_source(LPCSTR name)
{
    // Search
    string256 id;
    xr_strcpy(id, name);
    _strlwr(id);
    if (strext(id))
        *strext(id) = 0;

    std::scoped_lock lk(m_sources_lock);
    auto it = m_sources_map.find(id);
    if (it != m_sources_map.end())
    {
        g_snd_sources.hits++;
        return it->second;
    }

    // Load a _new one
    const auto t0 = std::chrono::steady_clock::now();
    CSoundRender_Source* S = xr_new<CSoundRender_Source>();
    S->load(id);
    s_sources.push_back(S);
    m_sources_map.emplace(id, S);
    g_snd_sources.loads++;
    g_snd_sources.load_us += u32(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    return S;
}

void CSoundRender_Core::i_destroy_source(CSoundRender_Source* S)
{
    // No actual destroy at all
}
