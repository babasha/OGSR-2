#include "StdAfx.h"
#include "sv_hit_trail.h"

#include "sv_world_lookup.h"
#include "xrServer_Objects.h"
#include "../xr_3da/NET_Server_Trash/NET_utils.h"
#include "../COMMON_AI/alife_space.h"

// Whether the power gate ran at all, which is what tells an empty 'capped'
// column from a cap that reached nothing. Defined in game_sv_base.cpp. ⚠At
// file scope: an `extern` inside a function in namespace SVHitTrail would
// declare SVHitTrail::g_sv_hit_validate and link to nothing.
extern int g_sv_hit_validate;

namespace
{
// Long enough to hold a firefight, short enough that it never becomes storage.
// A death asks about the last couple of seconds; nothing asks about the last
// minute, and if it ever does that is a different instrument.
constexpr u32 k_ring = 192;

struct Entry
{
    u32 ms{0};
    u16 victim{0xffff};
    u16 attacker{0xffff};
    u16 weapon{0xffff};
    u16 bone{0};
    u16 type{0};
    float power{0.f};
    bool dropped{false};
    bool network{false};
    u32 sender{0};
};

Entry s_ring[k_ring];
u32 s_written = 0; // total ever, so the ring's live span is knowable

// Per hit type, split by who claimed it. The split is the point: a number the
// server computed itself needs no cap, and one a remote client chose does.
struct TypeRow
{
    u32 n_local{0};
    u32 n_net{0};
    float max_local{0.f};
    float max_net{0.f};
    u16 worst_attacker{0xffff};
    u16 worst_weapon{0xffff};

    // What the gate could do about them, kept apart by side because the two
    // sides mean opposite things. A NETWORK claim over the ceiling is a client
    // asking for more than the world can make.
    u32 n_net_capped{0}; // a ceiling existed for this (source, type)
    u32 n_net_over{0}; // ...and the claim was above it

    // ⭐A SERVER hit over the ceiling is the ceiling being WRONG: the host
    // computed that number from the same content the cap was read out of. It
    // needs no cap and is counted anyway, because it is the one measurement
    // that says "do not switch this on yet" -- and nothing else would say it.
    u32 n_srv_capped{0};
    u32 n_srv_over{0};
};

TypeRow s_type[ALife::eHitTypeMax];

const Entry& at(u32 i) { return s_ring[i % k_ring]; }

u32 live() { return (s_written < k_ring) ? s_written : k_ring; }

LPCSTR name_of(u16 id)
{
    CSE_Abstract* e = SVWorld::Find(id);
    return e ? e->name_replace() : "?";
}

LPCSTR section_of(u16 id)
{
    CSE_Abstract* e = SVWorld::Find(id);
    return (e && e->s_name.size()) ? e->s_name.c_str() : "-";
}

LPCSTR type_name(u16 t) { return (t < ALife::eHitTypeMax) ? ALife::g_cafHitType2String(ALife::EHitType(t)) : "?"; }

void print_entry(const char* tag, const Entry& e, u32 now)
{
    Msg("~ [%s]   -%5u ms  %-11s power %.4f  bone %-3u  from %u ('%s') with '%s'%s%s", tag, now - e.ms, type_name(e.type), e.power, e.bone, e.attacker, name_of(e.attacker),
        section_of(e.weapon), e.dropped ? "  DROPPED (reached nobody)" : "", e.network ? "  [claimed by a network client]" : "");
}
} // namespace

void SVHitTrail::Note(NET_Packet& P, bool dropped, bool from_network, u32 sender)
{
    // SHit::Write_Packet_Cont's layout, read from the victim id -- the same
    // parse sv_health.cpp and the power cap do, and the cursor goes back where
    // it was because two more readers come after this one.
    const u32 saved = P.r_tell();

    u16 victim = 0xffff, attacker = 0xffff, weapon = 0xffff, bone = 0, aim = 0, hit_type = 0;
    Fvector dir, in_bone;
    float power = 0.f, impulse = 0.f;

    P.r_u16(victim);
    P.r_u16(attacker);
    P.r_u16(weapon);
    P.r_dir(dir);
    P.r_float(power);
    P.r_u16(bone);
    P.r_vec3(in_bone);
    P.r_float(impulse);
    P.r_u16(aim);
    P.r_u16(hit_type);

    P.r_seek(saved);

    Entry& e = s_ring[s_written % k_ring];
    e.ms = Device.dwTimeGlobal;
    e.victim = victim;
    e.attacker = attacker;
    e.weapon = weapon;
    e.bone = bone;
    e.type = hit_type;
    e.power = power;
    e.dropped = dropped;
    e.network = from_network;
    e.sender = sender;
    ++s_written;

    if (hit_type < ALife::eHitTypeMax)
    {
        TypeRow& r = s_type[hit_type];
        if (from_network)
        {
            ++r.n_net;
            if (power > r.max_net)
            {
                r.max_net = power;
                r.worst_attacker = attacker;
                r.worst_weapon = weapon;
            }
        }
        else
        {
            ++r.n_local;
            if (power > r.max_local)
                r.max_local = power;
        }
    }
}

void SVHitTrail::NoteCap(u16 hit_type, bool from_network, bool known, bool over)
{
    if (hit_type >= ALife::eHitTypeMax)
        return;

    TypeRow& r = s_type[hit_type];
    if (known)
        ++(from_network ? r.n_net_capped : r.n_srv_capped);
    if (over)
        ++(from_network ? r.n_net_over : r.n_srv_over);
}

u32 SVHitTrail::Count(u16 victim, u32 within_ms)
{
    const u32 now = Device.dwTimeGlobal;
    u32 n = 0;
    const u32 have = live();
    for (u32 i = 0; i < have; ++i)
    {
        const Entry& e = at(i);
        if (e.victim != victim || e.dropped)
            continue;
        if ((now - e.ms) <= within_ms)
            ++n;
    }
    return n;
}

void SVHitTrail::Print(u16 victim, u32 within_ms, const char* tag)
{
    const u32 now = Device.dwTimeGlobal;
    const u32 have = live();

    // Oldest first, so the sequence reads forwards. The ring's start is
    // wherever the write cursor is once it has wrapped.
    const u32 first = (s_written < k_ring) ? 0 : s_written;
    u32 shown = 0;
    for (u32 i = 0; i < have; ++i)
    {
        const Entry& e = at(first + i);
        if (e.victim != victim)
            continue;
        if ((now - e.ms) > within_ms)
            continue;
        print_entry(tag, e, now);
        ++shown;
    }

    if (!shown)
        Msg("~ [%s]   no hit of any kind reached the server for victim %u in the last %.1f s (%u hits recorded overall)", tag, victim, float(within_ms) / 1000.f, s_written);
}

void SVHitTrail::Dump(u16 victim)
{
    if (!s_written)
    {
        Msg("~ [sv_hit_trail] nothing recorded -- no hit has reached the server yet");
        return;
    }

    const u32 now = Device.dwTimeGlobal;
    const u32 have = live();
    const u32 first = (s_written < k_ring) ? 0 : s_written;

    Msg("~ [sv_hit_trail] ==== last %u of %u hits%s ====", have, s_written, (victim == 0xffff) ? "" : " (filtered)");
    for (u32 i = 0; i < have; ++i)
    {
        const Entry& e = at(first + i);
        if (victim != 0xffff && e.victim != victim)
            continue;
        Msg("~ [sv_hit_trail] -%6u ms  victim %5u ('%s')  %-11s power %.4f  bone %-3u  from %5u ('%s') with '%s'%s%s", now - e.ms, e.victim, name_of(e.victim), type_name(e.type),
            e.power, e.bone, e.attacker, name_of(e.attacker), section_of(e.weapon), e.dropped ? "  DROPPED" : "", e.network ? "  [net]" : "");
    }
}

void SVHitTrail::Types()
{
    Msg("~ [sv_hit_types] ==== what kinds of damage the server carried (%u hits) ====", s_written);
    Msg("~ [sv_hit_types] %-12s %8s %8s %10s %10s %8s %6s  %s", "type", "server", "network", "max srv", "max net", "capped", "over", "biggest network claim");

    u32 net_total = 0, net_capped = 0, net_over = 0, srv_over = 0;
    for (int t = 0; t < ALife::eHitTypeMax; ++t)
    {
        const TypeRow& r = s_type[t];
        if (!r.n_local && !r.n_net)
            continue;
        net_total += r.n_net;
        net_capped += r.n_net_capped;
        net_over += r.n_net_over;
        srv_over += r.n_srv_over;

        string256 worst;
        if (r.n_net)
            xr_sprintf(worst, "from %u ('%s') with '%s'", r.worst_attacker, name_of(r.worst_attacker), section_of(r.worst_weapon));
        else
            xr_strcpy(worst, "-");

        Msg("~ [sv_hit_types] %-12s %8u %8u %10.4f %10.4f %8u %6u  %s", type_name(u16(t)), r.n_local, r.n_net, r.max_local, r.max_net, r.n_net_capped, r.n_net_over, worst);
    }

    // ⭐The reading, spelled out, because the column that matters is the one
    // that is usually empty. Damage the SERVER produced needs no cap -- it
    // computed the number itself. Only the network column is a claim.
    Msg("~ [sv_hit_types] the 'server' column is damage the host computed itself (monsters, anomalies, scripts) -- nothing there needs a cap");

    if (!g_sv_hit_validate)
    {
        Msg("! [sv_hit_types] sv_hit_validate is 0, so the gate never ran: 'capped' and 'over' are zero because nobody asked, not because nothing was claimed");
        return;
    }

    // ⭐The uncapped remainder is the honest measure of what is still open --
    // and it is a count of CLAIMS, not of types. A type with a ceiling that
    // matched none of its sources reads as covered on any per-type list, and is
    // not.
    Msg("~ [sv_hit_types] of %u network claim(s) the server could compute a ceiling for %u; %u sat above it (%s). The remaining %u had no producer in content to bound them "
        "-- physics, scripts and creature melee are supposed to be there; see sv_cap_audit for what the content does declare",
        net_total, net_capped, net_over, (g_sv_hit_validate >= 2) ? "clamped" : "logged only, sv_hit_validate 1", net_total - net_capped);

    // ⭐The gate before the gate. The host's own hits are computed from the same
    // content the ceilings were read out of, so one of them landing above a
    // ceiling means the CEILING is wrong -- and clamping on a wrong ceiling
    // refuses honest damage, which is the failure this whole staged rollout
    // exists to avoid. ⚠sv_hit_fake counts here too, by design: it is injected
    // as the host's own.
    if (srv_over)
        Msg("!! [sv_hit_types] %u hit(s) the SERVER produced itself sat above the ceiling. Either they were faked (sv_hit_fake) or a ceiling is wrong -- do NOT set "
            "sv_hit_validate 2 until this is zero on a session nobody poked",
            srv_over);
}

void SVHitTrail::ResetTypes()
{
    for (int t = 0; t < ALife::eHitTypeMax; ++t)
        s_type[t] = TypeRow();
    Msg("~ [sv_hit_types] census cleared");
}

void SVHitTrail::Reset()
{
    s_written = 0;
    for (u32 i = 0; i < k_ring; ++i)
        s_ring[i] = Entry();
    for (int t = 0; t < ALife::eHitTypeMax; ++t)
        s_type[t] = TypeRow();
}
