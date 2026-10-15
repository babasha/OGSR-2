#include "StdAfx.h"
#include "PHCommander.h"

CPHCall::CPHCall(CPHCondition* condition, CPHAction* action)
{
    m_action = action;
    m_condition = condition;
    paused = 0;
    removed = false;
}

CPHCall::~CPHCall()
{
    xr_delete(m_action);
    xr_delete(m_condition);
}
bool CPHCall::obsolete() { return m_action->obsolete() || m_condition->obsolete(); }

void CPHCall::check()
{
    if (m_condition->is_true() && m_action)
        m_action->run();
}

bool CPHCall::equal(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action) { return m_action->compare(cmp_action) && m_condition->compare(cmp_condition); }
bool CPHCall::is_any(CPHReqComparerV* v) { return m_action->compare(v) || m_condition->compare(v); }

void CPHCall::setPause(u32 ms) { paused = Device.dwTimeGlobal + ms; }

bool CPHCall::isPaused() { return paused > Device.dwTimeGlobal; }

void CPHCall::removeLater() { removed = true; }

bool CPHCall::isNeedRemove() { return removed; }

/////////////////////////////////////////////////////////////////////////////////
CPHCommander::~CPHCommander() {}

void CPHCommander::clear()
{
    m_calls.clear();
}

void CPHCommander::update()
{
    // Snapshot the count: calls appended from inside check() run NEXT frame. Mods'
    // re-registration cadence (ogse_signals: a handler's action re-adds its own call)
    // only needs "next frame" — every already-queued call still runs every frame, so
    // nothing slows down. Draining appended calls same-frame instead lets a broken
    // handler with an instantly-true condition either spin this loop forever inside
    // ONE frame (frozen window, ~75GB of leaked luabind refs) or, when capped, grow
    // the queue without bound (seconds-per-frame lag). Verified both failure modes on
    // Living Zone content. Raw pointer, NOT a reference into the vector — add_call()
    // inside check() can reallocate m_calls and dangle a slot reference.
    const size_t count = m_calls.size();
    for (size_t it{}; it < count; ++it)
    {
        CPHCall* call = m_calls[it].get();
        if (!call->isNeedRemove() && !call->isPaused())
            call->check();
    }

    m_calls.erase(std::remove_if(m_calls.begin(), m_calls.end(), [](auto& call) { return call->isNeedRemove() || call->obsolete(); }), m_calls.end());

    // Hard backstop: no sane mod keeps thousands of live phys calls — beyond this
    // we're in leak territory (broken foreign-map scripts add a never-true call every
    // frame; each pins luabind refs in the LuaJIT heap AND costs a condition eval per
    // frame). Drop oldest. 8192 evals ≈ few ms — the frame stays playable.
    constexpr size_t kHardCap = 8192;
    if (m_calls.size() > kHardCap)
    {
        static u32 s_drops = 0;
        const u32 n = ++s_drops;
        if ((n & (n - 1)) == 0) // 1,2,4,8,…
            Msg("!![CPHCommander] %zu queued phys calls — dropping %zu oldest (mod-compat backstop, hit %u times)", m_calls.size(), m_calls.size() - kHardCap, n);
        m_calls.erase(m_calls.begin(), m_calls.begin() + (m_calls.size() - kHardCap));
    }
    // Leak canary — stateless (this method runs on TWO commander instances): shout when
    // the size crosses a power of two ≥ 4096.
    else if (m_calls.size() >= 4096 && (m_calls.size() & (m_calls.size() - 1)) == 0)
        Msg("!![CPHCommander] %zu queued phys calls — script call leak?", m_calls.size());

    // Leak attribution (r_profiler sessions only — g_seq_profile_cb is armed inside
    // seqFrame): every 10 s with >256 live calls, histogram them by the Lua
    // condition's "script.lua:line" (dump_source). The fps-decay bug grew THIS list
    // ~2k calls in minutes with the canary silent — this names the script doing it.
    if (g_seq_profile_cb && m_calls.size() > 256)
    {
        static u32 s_nextDump = 0;
        if (Device.dwTimeGlobal >= s_nextDump)
        {
            s_nextDump = Device.dwTimeGlobal + 10000;
            xr_map<xr_string, u32> hist;
            for (const auto& call : m_calls)
            {
                char src[160];
                call->condition()->dump_source(src, sizeof(src));
                ++hist[src[0] ? src : typeid(*call->condition()).name()];
            }
            Msg("[CPHCommander] %zu live calls by condition source:", m_calls.size());
            xr_vector<std::pair<u32, const xr_string*>> top;
            top.reserve(hist.size());
            for (const auto& kv : hist) top.emplace_back(kv.second, &kv.first);
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < top.size() && i < 8; ++i)
                Msg("  %ux %s", top[i].first, top[i].second->c_str());

            // One-shot: print the source window around the top offender's line —
            // mod scripts live inside encrypted .db archives only the engine can
            // read, so this is the practical way to SEE the leaking code.
            static bool s_srcDumped = false;
            if (!s_srcDumped && !top.empty())
            {
                s_srcDumped = true;
                const char* full  = top[0].second->c_str();
                const char* colon = strrchr(full, ':');
                if (colon)
                {
                    const int hot = atoi(colon + 1);
                    xr_string path(full, colon - full);
                    const size_t sl = path.find_last_of("\\/");
                    const xr_string base = (sl == xr_string::npos) ? path : path.substr(sl + 1);
                    if (IReader* r = FS.r_open("$game_scripts$", base.c_str()))
                    {
                        Msg("[CPHCommander] source window %s around line %d:", base.c_str(), hot);
                        int ln = 1;
                        string4096 line;
                        while (!r->eof() && ln <= hot + 15)
                        {
                            r->r_string(line, sizeof(line));
                            if (ln >= hot - 12)
                                Msg("  %c%4d| %s", ln == hot ? '>' : ' ', ln, line);
                            ++ln;
                        }
                        // Full copy to appdata: mod scripts live in encrypted .db
                        // archives — this is the practical way to get the whole
                        // file out for analysis / a loose-override fix.
                        r->seek(0);
                        string_path dumpName;
                        xr_sprintf(dumpName, "dump_%s", base.c_str());
                        if (IWriter* w = FS.w_open("$app_data_root$", dumpName))
                        {
                            w->w(r->pointer(), r->length());
                            FS.w_close(w);
                            Msg("[CPHCommander] full source dumped to appdata: %s", dumpName);
                        }
                        FS.r_close(r);
                    }
                    else
                        Msg("![CPHCommander] can't open %s via $game_scripts$", base.c_str());
                }
            }
        }
    }
}

CPHCall* CPHCommander::add_call(CPHCondition* condition, CPHAction* action) { return m_calls.emplace_back(std::make_unique<CPHCall>(condition, action)).get(); }

PHCALL_I CPHCommander::find_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
    return std::find_if(m_calls.begin(), m_calls.end(), [&](auto& call) { return !call->isNeedRemove() && call->equal(cmp_condition, cmp_action); });
}

bool CPHCommander::has_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action) { return find_call(cmp_condition, cmp_action) != m_calls.end(); }

void CPHCommander::remove_call(CPHReqComparerV* cmp_condition, CPHReqComparerV* cmp_action)
{
    auto it = find_call(cmp_condition, cmp_action);
    if (it != m_calls.end())
    {
        auto call = it->get();
        call->removeLater();
    }
}

CPHCall* CPHCommander::add_call_unique(CPHCondition* condition, CPHReqComparerV* cmp_condition, CPHAction* action, CPHReqComparerV* cmp_action)
{
    auto it = find_call(cmp_condition, cmp_action);
    if (it == m_calls.end())
        return add_call(condition, action);
    return it->get();
}

void CPHCommander::remove_calls(CPHReqComparerV* cmp_object)
{
    for (const auto& call : m_calls)
        if (!call->isNeedRemove() && call->is_any(cmp_object))
            call->removeLater();
}
