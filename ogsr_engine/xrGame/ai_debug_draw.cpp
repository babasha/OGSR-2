#include "StdAfx.h"
#include "ai_debug_draw.h"

#include "Level.h"
#include "HUDManager.h"
#include "cl_zone_debug.h"
#include "ai/stalker/ai_stalker.h"
#include "stalker_combat_actions.h"
#include "../xr_3da/GameFont.h"

int AIDebugDraw::g_mask = 0;
float AIDebugDraw::g_range = 60.f;

namespace
{
using namespace AIDebugDraw;

enum EKind : u8
{
    eKindLine,
    eKindCross,
    eKindRing,
    eKindText,
};

struct SItem
{
    Fvector a, b;
    u32 color;
    u32 expire; // level time; 0 = this frame only
    float size;
    EKind kind;
    char text[96];
};

struct SCandidate
{
    Fvector p;
    float score;
    u32 time;
    u16 owner;
    u8 channel;
    char verdict;
};

constexpr u32 MAX_ITEMS = 4096;
constexpr u32 MAX_CANDIDATES = 2048;
constexpr u32 CANDIDATE_TTL = 5000; // a choice stays on screen this long after it was made
// Segments per frame. Six vertices each through the UI ring (8 MB, shared with
// the rest of the UI): 6000 is about 1 MB of it. Past that, the rest is dropped
// and the count says so on screen.
constexpr u32 MAX_SEGMENTS = 6000;
constexpr u32 RING_SEGMENTS = 24;
constexpr float LABEL_RANGE = 20.f; // candidate scores are written only this near the camera: farther they pile up unreadably

xr_vector<SItem> s_items;
xr_vector<SCandidate> s_candidates;
xr_map<u32, u32> s_stamps; // owner << 8 | channel -> the stamp of the set on screen

void push(EKind kind, const Fvector& a, const Fvector& b, float size, u32 color, u32 ttl_ms)
{
    if (s_items.size() >= MAX_ITEMS)
        return;
    SItem& item = s_items.emplace_back();
    item.a = a;
    item.b = b;
    item.size = size;
    item.color = color;
    item.kind = kind;
    item.expire = ttl_ms ? Device.dwTimeGlobal + ttl_ms : 0;
    item.text[0] = 0;
}

u32 segments_of(const SItem& item)
{
    switch (item.kind)
    {
    case eKindLine: return 1;
    case eKindCross: return 3;
    case eKindRing: return RING_SEGMENTS;
    default: return 0;
    }
}

bool in_range(const Fvector& eye, const Fvector& p) { return eye.distance_to_sqr(p) <= _sqr(g_range); }

void draw_item(const SItem& item)
{
    switch (item.kind)
    {
    case eKindLine: ClZoneDebug::DrawLine(item.a, item.b, item.color); break;
    case eKindCross:
        for (u32 axis = 0; axis < 3; ++axis)
        {
            Fvector d{};
            d[axis] = item.size;
            ClZoneDebug::DrawLine(Fvector(item.a).sub(d), Fvector(item.a).add(d), item.color);
        }
        break;
    case eKindRing:
    {
        Fvector prev{};
        for (u32 i = 0; i <= RING_SEGMENTS; ++i)
        {
            const float angle = PI_MUL_2 * float(i) / float(RING_SEGMENTS);
            const Fvector p{item.a.x + item.size * _cos(angle), item.a.y, item.a.z + item.size * _sin(angle)};
            if (i)
                ClZoneDebug::DrawLine(prev, p, item.color);
            prev = p;
        }
        break;
    }
    default: break;
    }
}

// What threw a candidate out, as written next to it.
constexpr LPCSTR LEGEND = "candidates: number = score (lower wins, white = chosen) | d distance window, p way runs past the enemy, c refused by the caller, "
                          "f not in front, r unreachable, n near another (post), v sees nothing in, i inside the house, e seen crouched there, w wall in the face, - never looked at";

// The candidates go in as items of this frame: the best of each set picked out,
// a faint line from it to the man who chose.
void candidates_to_items(const Fvector& eye)
{
    // the best scored point of each set
    xr_map<u32, const SCandidate*> best;
    for (const SCandidate& c : s_candidates)
    {
        if (c.verdict != ' ')
            continue;
        const u32 key = (u32(c.owner) << 8) | c.channel;
        auto [I, fresh] = best.try_emplace(key, &c);
        if (!fresh && c.score < I->second->score)
            I->second = &c;
    }

    static constexpr u32 channel_color[eChannelCount] = {color_rgba(255, 220, 60, 230), color_rgba(80, 220, 255, 230), color_rgba(255, 170, 60, 230), color_rgba(120, 255, 180, 230), color_rgba(120, 160, 255, 230),
                                                         color_rgba(255, 120, 220, 230), color_rgba(60, 255, 120, 230), color_rgba(200, 200, 120, 230)};
    for (const SCandidate& c : s_candidates)
    {
        if (!in_range(eye, c.p))
            continue;
        const u32 key = (u32(c.owner) << 8) | c.channel;
        const auto I = best.find(key);
        const bool chosen = I != best.end() && I->second == &c;
        const Fvector p = Fvector(c.p).add(Fvector{0.f, .1f, 0.f});
        if (c.verdict != ' ')
            push(eKindCross, p, p, .12f, c.verdict == '-' ? color_rgba(170, 170, 170, 120) : color_rgba(255, 70, 60, 150), 0);
        else
            push(eKindCross, p, p, chosen ? .45f : .22f, chosen ? color_rgba(255, 255, 255, 255) : channel_color[c.channel], 0);

        if (chosen)
        {
            push(eKindRing, p, p, .5f, color_rgba(255, 255, 255, 220), 0);
            if (CObject* owner = Level().Objects.net_Find(c.owner))
                push(eKindLine, Fvector(owner->Position()).add(Fvector{0.f, .1f, 0.f}), p, 0.f, color_rgba(255, 255, 255, 90), 0);
        }

        if (eye.distance_to_sqr(c.p) > _sqr(LABEL_RANGE) || s_items.size() >= MAX_ITEMS)
            continue;
        SItem& label = s_items.emplace_back();
        label = {};
        label.kind = eKindText;
        label.a = Fvector(c.p).add(Fvector{0.f, .35f, 0.f});
        label.color = c.verdict != ' ' ? color_rgba(255, 110, 100, 200) : chosen ? color_rgba(255, 255, 255, 255) : channel_color[c.channel];
        if (c.verdict != ' ')
            xr_sprintf(label.text, "%c", c.verdict);
        else
            xr_sprintf(label.text, "%.2f", c.score);
    }
}
} // namespace

void AIDebugDraw::Line(const Fvector& a, const Fvector& b, u32 color, u32 ttl_ms) { push(eKindLine, a, b, 0.f, color, ttl_ms); }

void AIDebugDraw::Cross(const Fvector& p, float size, u32 color, u32 ttl_ms) { push(eKindCross, p, p, size, color, ttl_ms); }

void AIDebugDraw::Ring(const Fvector& centre, float radius, u32 color, u32 ttl_ms) { push(eKindRing, centre, centre, radius, color, ttl_ms); }

void AIDebugDraw::Text(const Fvector& p, u32 color, u32 ttl_ms, LPCSTR fmt, ...)
{
    if (s_items.size() >= MAX_ITEMS)
        return;
    push(eKindText, p, p, 0.f, color, ttl_ms);
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_items.back().text, sizeof(s_items.back().text), fmt, args);
    va_end(args);
}

void AIDebugDraw::Candidate(u16 owner, u8 channel, u32 stamp, const Fvector& p, float score, char verdict)
{
    if (channel >= eChannelCount)
        return;
    // a new choice by the same man in the same channel: the old set goes
    const u32 key = (u32(owner) << 8) | channel;
    auto [I, fresh] = s_stamps.try_emplace(key, stamp);
    if (!fresh && I->second != stamp)
    {
        I->second = stamp;
        s_candidates.erase(std::remove_if(s_candidates.begin(), s_candidates.end(), [&](const SCandidate& c) { return c.owner == owner && c.channel == channel; }),
                           s_candidates.end());
    }
    if (s_candidates.size() >= MAX_CANDIDATES)
        s_candidates.erase(s_candidates.begin()); // the oldest
    s_candidates.push_back({p, score, Device.dwTimeGlobal, owner, channel, verdict});
}

void AIDebugDraw::Frame()
{
    if (!g_mask)
    {
        // switched off: nothing kept, nothing left to cost anything
        if (!s_items.empty() || !s_candidates.empty())
        {
            s_items.clear();
            s_candidates.clear();
            s_stamps.clear();
        }
        return;
    }
    if (!g_pGameLevel || !Level().game_configured)
        return;

    // Outside the render pass on purpose: a UI shader created inside it came
    // back with no texture (cl_zone_debug.cpp).
    ClZoneDebug::EnsureShaders();

    const u32 now = Device.dwTimeGlobal;
    s_items.erase(std::remove_if(s_items.begin(), s_items.end(), [now](const SItem& item) { return item.expire && item.expire < now; }), s_items.end());
    s_candidates.erase(std::remove_if(s_candidates.begin(), s_candidates.end(), [now](const SCandidate& c) { return c.time + CANDIDATE_TTL < now; }),
                       s_candidates.end());
    if (s_candidates.empty())
        s_stamps.clear();
}

void AIDebugDraw::Render()
{
    if (!g_mask || !g_pGameLevel || !Level().game_configured || !UIRender)
        return;

    const Fvector eye = Device.vCameraPosition;

    // what is decided at draw time: the men and the houses, as of now
    stalker_dbg_draw(eye, g_range);
    if (on(eCandidates))
        candidates_to_items(eye);

    u32 segments = 0, dropped = 0;
    for (const SItem& item : s_items)
        if (item.kind != eKindText && in_range(eye, item.a))
            segments += segments_of(item);
    if (segments > MAX_SEGMENTS)
    {
        dropped = segments - MAX_SEGMENTS;
        segments = MAX_SEGMENTS;
    }

    if (segments)
    {
        ClZoneDebug::BeginLines(segments);
        u32 drawn = 0;
        for (const SItem& item : s_items)
        {
            if (item.kind == eKindText || !in_range(eye, item.a))
                continue;
            const u32 n = segments_of(item);
            if (drawn + n > segments)
                break;
            draw_item(item);
            drawn += n;
        }
        ClZoneDebug::EndLines();
    }

    if (CGameFont* F = HUD().Font().pFontSmall)
    {
        F->SetAligment(CGameFont::alCenter);
        for (const SItem& item : s_items)
        {
            if (item.kind != eKindText || !item.text[0] || !in_range(eye, item.a))
                continue;
            float sx, sy;
            if (!ClZoneDebug::ToScreen(item.a, sx, sy))
                continue;
            F->SetColor(item.color);
            // two lines at most: the second after a '\n'
            if (LPCSTR nl = strchr(item.text, '\n'))
            {
                string64 first{};
                strncpy_s(first, item.text, size_t(nl - item.text));
                F->Out(sx, sy, "%s", first);
                F->Out(sx, sy + 13.f, "%s", nl + 1);
            }
            else
                F->Out(sx, sy, "%s", item.text);
        }
        F->SetAligment(CGameFont::alLeft);
        if (on(eCandidates))
        {
            F->SetColor(color_rgba(220, 220, 220, 220));
            F->Out(12.f, float(Device.dwHeight) - 40.f, "%s", LEGEND);
        }
        if (dropped)
        {
            F->SetColor(color_rgba(255, 90, 60, 255));
            F->Out(12.f, float(Device.dwHeight) - 56.f, "ai_dbg_draw: %u segments over the budget not drawn -- lower ai_dbg_draw_range", dropped);
        }
    }

    // this frame's items are spent
    s_items.erase(std::remove_if(s_items.begin(), s_items.end(), [](const SItem& item) { return !item.expire; }), s_items.end());
}
