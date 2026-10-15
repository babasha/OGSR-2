#include "StdAfx.h"
#include "sv_telemetry.h"
#include "sv_world_lookup.h"

#include "xrServer.h"
#include "xrServer_Objects.h"
#include "game_base.h"
#include "game_sv_base.h"
#include "Level.h"
#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Items.h"
#include "xrServer_Objects_ALife_Monsters.h"

#include <stdio.h>
#include <time.h>

// Knobs -- see sv_telemetry.h for what each one does. Default off: this is
// stock behaviour for a process that merely happens to own a server (a
// single-player session does), and the dedicated host turns it on at boot.
int g_sv_telemetry = 0;
float g_sv_telemetry_census = 60.f;
int g_sv_telemetry_stock = 1;
int g_sv_telemetry_pos = 1;

namespace
{
FILE* s_file = nullptr;
string_path s_fname = {};
bool s_open_failed = false;
u32 s_seq = 0;
u32 s_open_time = 0;
u32 s_last_census = 0;
u32 s_last_flush = 0;
bool s_dirty = false;

// A hit remembers which weapon caused it so that a death, which carries no
// weapon at all, can be attributed to one. Open-addressed by victim id --
// collisions just lose an attribution, and entries go stale on their own,
// so nothing here has to be cleaned up on level change or on death.
constexpr u32 LAST_HIT_SLOTS = 128;
constexpr u32 LAST_HIT_TTL_MS = 15000;

// How long after the file opens the first census waits. Measured, not chosen:
// game->Update() ticks during the blocking level load, and the ALife population
// pass finishes a couple of seconds AFTER the level is otherwise ready -- a
// census taken at t=1s reported 1792 objects and one single creature on a map
// that has 1990 and 82. There is no engine flag for "the world is populated",
// so this is a settle window rather than a condition.
constexpr u32 FIRST_CENSUS_DELAY_MS = 5000;

struct LastHit
{
    u16 victim{0xffff};
    u16 attacker{0xffff};
    u16 hit_type{0};
    float power{0.f};
    float ap{0.f};
    u32 time{0};
    shared_str weapon;
    shared_str ammo;
};
LastHit s_last_hits[LAST_HIT_SLOTS];

u64 game_time()
{
    xrServer* SV = SVWorld::Server();
    return (SV && SV->game) ? u64(SV->game->GetGameTime()) : 0ull;
}

// Which round a weapon is loaded with. The server holds this: a weapon's
// server entity carries an index into its own ammo_class list. Without it a
// hit names the gun but not the ammunition, and the two are different
// economies -- the same rifle firing AP and firing FMJ consumes different
// stock, sells for different money and kills at different rates. "Ammunition
// spent, by type" cannot be answered from a weapon section alone.
shared_str ammo_of(CSE_Abstract* weapon)
{
    CSE_ALifeItemWeapon* w = smart_cast<CSE_ALifeItemWeapon*>(weapon);
    if (!w || !weapon->s_name.size())
        return shared_str();

    LPCSTR sect = weapon->s_name.c_str();
    if (!pSettings->section_exist(sect) || !pSettings->line_exist(sect, "ammo_class"))
        return shared_str();

    LPCSTR list = pSettings->r_string(sect, "ammo_class");
    if (!list || !list[0])
        return shared_str();

    const int count = _GetItemCount(list);
    int idx = int(w->ammo_type);
    if (idx < 0 || idx >= count)
        idx = 0; // an unset or stale index still names the weapon's default round

    string128 buffer;
    return shared_str(_GetItem(list, idx, buffer));
}

// ---------------------------------------------------------------------
// Record builder. Fixed buffer, no allocation, truncation-safe: a record
// that would overflow is closed early and marked, never written past the
// end of the buffer.
// ---------------------------------------------------------------------
class Rec
{
    string4096 m_buf;
    int m_len{0};
    bool m_full{false};

    void put(const char* s)
    {
        const int cap = int(sizeof(m_buf)) - 4; // room for }, \n, \0
        while (*s)
        {
            if (m_len >= cap)
            {
                m_full = true;
                return;
            }
            m_buf[m_len++] = *s++;
        }
    }

    void put_quoted(const char* s)
    {
        put("\"");
        if (s)
        {
            const int cap = int(sizeof(m_buf)) - 8;
            for (; *s && m_len < cap; ++s)
            {
                const unsigned char c = (unsigned char)*s;
                if (c == '"' || c == '\\')
                {
                    m_buf[m_len++] = '\\';
                    m_buf[m_len++] = char(c);
                }
                else if (c < 0x20)
                {
                    // Control characters are not legal raw in JSON strings,
                    // and player names arrive from the wire.
                    m_len += sprintf_s(m_buf + m_len, sizeof(m_buf) - m_len, "\\u%04x", c);
                }
                else
                    m_buf[m_len++] = char(c);
            }
        }
        put("\"");
    }

    void key(const char* k)
    {
        if (m_len > 1)
            put(",");
        put_quoted(k);
        put(":");
    }

    static float sane(float v)
    {
        // NaN and infinities are not representable in JSON; one of them in
        // one field would make the whole line unparseable.
        if (!(v == v) || v > 1e30f || v < -1e30f)
            return 0.f;
        return v;
    }

public:
    explicit Rec(const char* kind)
    {
        m_buf[0] = '{';
        m_len = 1;
        S("ev", kind);
        U("n", ++s_seq);
        U("t", Device.dwTimeGlobal);
        U("gt", game_time());
    }

    Rec& S(const char* k, const char* v)
    {
        key(k);
        put_quoted(v);
        return *this;
    }
    Rec& U(const char* k, u64 v)
    {
        key(k);
        string32 tmp;
        sprintf_s(tmp, "%llu", v);
        put(tmp);
        return *this;
    }
    Rec& I(const char* k, s64 v)
    {
        key(k);
        string32 tmp;
        sprintf_s(tmp, "%lld", v);
        put(tmp);
        return *this;
    }
    Rec& F(const char* k, float v)
    {
        key(k);
        string32 tmp;
        sprintf_s(tmp, "%.3f", sane(v));
        put(tmp);
        return *this;
    }
    Rec& B(const char* k, bool v)
    {
        key(k);
        put(v ? "true" : "false");
        return *this;
    }
    Rec& V(const char* k, const Fvector& v)
    {
        key(k);
        string64 tmp;
        sprintf_s(tmp, "[%.2f,%.2f,%.2f]", sane(v.x), sane(v.y), sane(v.z));
        put(tmp);
        return *this;
    }
    // Already-formatted JSON body, no leading comma. See SVTelemetry::Raw.
    Rec& Body(const char* fields)
    {
        if (fields && *fields)
        {
            if (m_len > 1)
                put(",");
            put(fields);
        }
        return *this;
    }

    void Emit()
    {
        if (!s_file)
            return;
        if (m_full)
        {
            // Say so in the record itself rather than silently writing a
            // half line: a reader must be able to tell it lost fields.
            const char* mark = ",\"trunc\":true";
            const int room = int(sizeof(m_buf)) - m_len - 3;
            if (room > int(xr_strlen(mark)))
            {
                strcpy_s(m_buf + m_len, sizeof(m_buf) - m_len, mark);
                m_len += int(xr_strlen(mark));
            }
        }
        m_buf[m_len++] = '}';
        m_buf[m_len++] = '\n';
        fwrite(m_buf, 1, size_t(m_len), s_file);
        s_dirty = true;
    }
};

// ---------------------------------------------------------------------
// Entity description helpers
// ---------------------------------------------------------------------

// A client-controlled entity, as opposed to one the server simulates. The
// server's own synthetic client owns every NPC, so ownership alone says
// nothing -- the set of entities a connected client exports state for is
// the honest test.
bool is_player_entity(CSE_Abstract* e)
{
    xrServer* SV = SVWorld::Server();
    if (!SV || !e)
        return false;
    if (SV->net_client_controls(e->ID))
        return true;
    // The base ALife actor is handed to whichever client adopts it, and it
    // is the one entity a client can own without being in that set.
    return (e->owner != nullptr) && (e->ID == 0);
}

const char* player_name_of(CSE_Abstract* e)
{
    xrServer* SV = SVWorld::Server();
    if (!SV || !e)
        return nullptr;
    for (u32 i = 0; i < SV->client_Count(); ++i)
    {
        xrClientData* CL = (xrClientData*)SV->client_Get(i);
        if (CL && CL->owner == e)
            return CL->ps ? CL->ps->getName() : (*CL->name ? *CL->name : nullptr);
    }
    return nullptr;
}

// "" when the entity has no community (monsters, items, anything that is
// not a character). CommunityName() indexes a table, so an unset index has
// to be filtered out before asking.
const char* community_of(CSE_Abstract* e)
{
    CSE_ALifeTraderAbstract* t = smart_cast<CSE_ALifeTraderAbstract*>(e);
    if (t && t->Community() != NO_COMMUNITY_INDEX)
        return t->CommunityName();
    return nullptr;
}

// Fields describing one entity, prefixed so several can share a record.
void add_entity(Rec& r, const char* pfx, CSE_Abstract* e)
{
    string32 k;
    if (!e)
        return;
    sprintf_s(k, "%sid", pfx);
    r.U(k, e->ID);
    sprintf_s(k, "%ssect", pfx);
    r.S(k, e->s_name.c_str());
    if (const char* comm = community_of(e))
    {
        sprintf_s(k, "%scomm", pfx);
        r.S(k, comm);
    }
    if (is_player_entity(e))
    {
        sprintf_s(k, "%spl", pfx);
        r.B(k, true);
        if (const char* nm = player_name_of(e))
        {
            sprintf_s(k, "%sname", pfx);
            r.S(k, nm);
        }
    }
    if (CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(e))
    {
        sprintf_s(k, "%shp", pfx);
        r.F(k, c->g_Health());
    }
}

// Who ultimately holds an item: walk the parent chain to its root, because
// ammo inside a weapon inside a stalker is held by the stalker, not by the
// weapon. Depth-capped -- a cycle in the chain must not hang the census.
void holder_of(CSE_Abstract* item, string128& out)
{
    strcpy_s(out, "ground");
    if (!item)
        return;

    CSE_Abstract* root = nullptr;
    u16 parent = item->ID_Parent;
    for (int depth = 0; depth < 8 && 0xffff != parent; ++depth)
    {
        CSE_Abstract* p = SVWorld::Find(parent);
        if (!p)
        {
            strcpy_s(out, "unknown");
            return;
        }
        root = p;
        parent = p->ID_Parent;
    }
    if (!root)
        return; // no parent at all: lying in the world

    if (is_player_entity(root))
    {
        const char* nm = player_name_of(root);
        sprintf_s(out, "player:%s", nm ? nm : "?");
        return;
    }
    if (const char* comm = community_of(root))
    {
        strcpy_s(out, comm);
        return;
    }
    if (smart_cast<CSE_ALifeCreatureAbstract*>(root))
    {
        strcpy_s(out, "creature");
        return;
    }
    // Anything else that can hold items is a container: a box, a stash, a
    // corpse's inventory carrier. Keep its section -- which container
    // matters as much as how much is in it.
    sprintf_s(out, "box:%s", root->s_name.c_str());
}

// Category an item counts under. Order matters: ammo, weapons and
// artefacts all derive from CSE_ALifeItem, so the catch-all goes last.
// Everything else (outfits, medkits, food, quest items) lands in "item" --
// with the section recorded, finer classes are a question for the reader,
// not for the engine.
const char* item_category(CSE_Abstract* e, u32& rounds)
{
    rounds = 0;
    if (CSE_ALifeItemAmmo* a = smart_cast<CSE_ALifeItemAmmo*>(e))
    {
        rounds = a->a_elapsed;
        return "ammo";
    }
    if (smart_cast<CSE_ALifeItemWeapon*>(e))
        return "weapon";
    if (smart_cast<CSE_ALifeItemArtefact*>(e))
        return "artefact";
    if (smart_cast<CSE_ALifeItem*>(e))
        return "item";
    return nullptr;
}

// ---------------------------------------------------------------------
// File lifecycle
// ---------------------------------------------------------------------
bool ensure_open()
{
    if (!g_sv_telemetry)
        return false;
    if (s_file)
        return true;
    if (s_open_failed)
        return false;
    if (!SVWorld::Server())
        return false; // no server here: nothing authoritative to write

    // Measured, not guessed: game->Update() (and therefore Frame()) ticks during
    // the blocking level load, so without this gate the first census lands on a
    // half-filled ALife registry and reports a world with one creature in it.
    // A world that is not there yet must not be described.
    if (ai().get_alife() && !ai().alife().initialized())
        return false;

    time_t now = time(nullptr);
    tm lt = {};
    localtime_s(&lt, &now);

    string_path leaf;
    sprintf_s(leaf, "telemetry_%04d%02d%02d_%02d%02d%02d.jsonl", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
    FS.update_path(s_fname, "$logs$", leaf);

    // Plain stdio on purpose: this log has to survive the process dying,
    // so writes must be flushable on demand rather than buffered until
    // some owner object is destroyed.
    s_file = fopen(s_fname, "ab");
    if (!s_file)
    {
        s_open_failed = true;
        Msg("! [sv_telemetry] cannot open '%s' -- telemetry disabled for this run", s_fname);
        return false;
    }

    Msg("* [sv_telemetry] writing '%s'", s_fname);

    Rec r("session");
    r.S("world", g_pGameLevel ? Level().name().c_str() : "?");
    r.U("pid", u64(GetCurrentProcessId()));
    {
        string64 stamp;
        sprintf_s(stamp, "%04d-%02d-%02dT%02d:%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
        r.S("wall", stamp);
    }
    r.I("level", g_sv_telemetry);
    r.F("census", g_sv_telemetry_census);
    r.Emit();

    s_open_time = Device.dwTimeGlobal;
    s_last_census = 0; // first census as soon as the settle window is over
    s_last_flush = Device.dwTimeGlobal;
    return true;
}

// ---------------------------------------------------------------------
// Census
// ---------------------------------------------------------------------
struct Pop
{
    u32 total{0}, alive{0}, dead{0}, online{0};
};
struct Stock
{
    u32 n{0}, rounds{0};
    xr_string holder, cat, sect;
};

void census()
{
    xrServer* SV = SVWorld::Server();
    if (!SV)
        return;

    u32 players = 0, net_players = 0;
    for (u32 i = 0; i < SV->client_Count(); ++i)
    {
        xrClientData* CL = (xrClientData*)SV->client_Get(i);
        if (!CL)
            continue;
        ++players;
        if (!CL->flags.bLocal)
            ++net_players;

        if (!g_sv_telemetry_pos)
            continue;

        // One position per player per census. This is the record that makes
        // "who was where, when" answerable after the fact -- including for
        // areas that were not yet interesting when the sample was taken.
        Rec r("pos");
        r.U("cid", CL->ID.value());
        r.S("name", CL->ps ? CL->ps->getName() : (*CL->name ? *CL->name : "?"));
        if (CL->flags.bLocal)
            r.B("local", true); // the server's own synthetic client, not a person
        if (CSE_Abstract* E = CL->owner)
        {
            r.U("eid", E->ID);
            r.V("p", E->o_Position);
            if (CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(E))
                r.F("hp", c->g_Health());
        }
        r.Emit();
    }

    u32 objects = 0, online = 0, creatures = 0, alive = 0, items = 0;
    xr_map<xr_string, Pop> pop;
    xr_map<xr_string, Stock> stock;

    if (ai().get_alife())
    {
        for (const auto& it : ai().alife().objects().objects())
        {
            CSE_ALifeDynamicObject* O = it.second;
            if (!O)
                continue;
            ++objects;
            if (O->m_bOnline)
                ++online;

            if (CSE_ALifeCreatureAbstract* c = smart_cast<CSE_ALifeCreatureAbstract*>(O))
            {
                ++creatures;
                const bool is_alive = c->g_Alive();
                if (is_alive)
                    ++alive;

                const char* comm = community_of(O);
                Pop& p = pop[comm ? comm : "monster"];
                ++p.total;
                if (is_alive)
                    ++p.alive;
                else
                    ++p.dead;
                if (O->m_bOnline)
                    ++p.online;
                continue;
            }

            u32 rounds = 0;
            const char* cat = item_category(O, rounds);
            if (!cat)
                continue;
            ++items;

            if (!g_sv_telemetry_stock)
                continue;

            string128 holder;
            holder_of(O, holder);

            xr_string key(holder);
            key += "\x1f";
            key += cat;
            key += "\x1f";
            key += O->s_name.c_str();

            Stock& s = stock[key];
            if (!s.n)
            {
                s.holder = holder;
                s.cat = cat;
                s.sect = O->s_name.c_str();
            }
            ++s.n;
            s.rounds += rounds;
        }
    }

    {
        Rec r("census");
        r.U("players", players);
        r.U("net", net_players);
        r.U("objects", objects);
        r.U("online", online);
        r.U("creatures", creatures);
        r.U("alive", alive);
        r.U("items", items);
        r.Emit();
    }

    for (const auto& p : pop)
    {
        Rec r("pop");
        r.S("comm", p.first.c_str());
        r.U("total", p.second.total);
        r.U("alive", p.second.alive);
        r.U("dead", p.second.dead);
        r.U("online", p.second.online);
        r.Emit();
    }

    for (const auto& s : stock)
    {
        Rec r("stock");
        r.S("holder", s.second.holder.c_str());
        r.S("cat", s.second.cat.c_str());
        r.S("sect", s.second.sect.c_str());
        r.U("n", s.second.n);
        if (s.second.cat == "ammo")
            r.U("rounds", s.second.rounds);
        r.Emit();
    }
}
} // namespace

// =====================================================================
// Public interface
// =====================================================================
bool SVTelemetry::Active() { return g_sv_telemetry != 0 && s_file != nullptr; }

const char* SVTelemetry::FileName() { return s_file ? s_fname : ""; }

void SVTelemetry::Close()
{
    if (!s_file)
        return;
    Rec r("close");
    r.Emit();
    fclose(s_file);
    s_file = nullptr;
    s_dirty = false;
    Msg("* [sv_telemetry] closed '%s'", s_fname);
}

void SVTelemetry::Frame()
{
    if (!g_sv_telemetry)
    {
        // Turned off at runtime: release the file so it can be read while
        // the server keeps running.
        if (s_file)
            Close();
        return;
    }
    if (!ensure_open())
        return;

    const u32 now = Device.dwTimeGlobal;

    if (g_sv_telemetry_census > 0.f && (now - s_open_time) >= FIRST_CENSUS_DELAY_MS)
    {
        const u32 period = u32(g_sv_telemetry_census * 1000.f);
        if (!s_last_census || (now - s_last_census) >= period)
        {
            s_last_census = now;
            census();
        }
    }

    // A second's worth of records is the most a crash may cost.
    if (s_dirty && (now - s_last_flush) >= 1000)
    {
        fflush(s_file);
        s_dirty = false;
        s_last_flush = now;
    }
}

void SVTelemetry::CensusNow()
{
    if (!ensure_open())
        return;
    s_last_census = Device.dwTimeGlobal;
    census();
    fflush(s_file);
    s_dirty = false;
}

void SVTelemetry::Raw(const char* kind, const char* fields)
{
    if (!ensure_open())
        return;
    Rec r(kind ? kind : "raw");
    r.Body(fields);
    r.Emit();
}

void SVTelemetry::OnClient(const char* what, u32 client_id, const char* name, CSE_Abstract* entity, bool local)
{
    if (!ensure_open())
        return;
    Rec r("client");
    r.S("what", what);
    r.U("cid", client_id);
    r.S("name", name ? name : "?");
    if (local)
        r.B("local", true);
    if (entity)
    {
        r.U("eid", entity->ID);
        r.V("p", entity->o_Position);
    }
    r.Emit();
}

void SVTelemetry::OnHit(NET_Packet& P, bool dropped)
{
    if (!ensure_open())
        return;

    // Layout is SHit::Write_Packet_Cont's, read from where the queued event
    // left its cursor (victim id first, see xrServer::Process_event GE_HIT).
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

    // Armour piercing rides along only for bullets, and it comes straight off
    // the round's k_ap -- so it is a property of the ammunition, not the gun.
    float ap = 0.f;
    if (ALife::eHitTypeFireWound == hit_type)
        P.r_float(ap);

    P.r_seek(saved); // the real handler still has to read this packet

    CSE_Abstract* e_victim = SVWorld::Find(victim);
    CSE_Abstract* e_attacker = SVWorld::Find(attacker);
    CSE_Abstract* e_weapon = SVWorld::Find(weapon);

    // Remember the weapon even at level 1, where individual hits are not
    // logged: a death record with no weapon is most of the value lost. A
    // dropped hit is deliberately kept OUT of this cache -- it took nobody's
    // health, so it cannot be what killed them.
    const shared_str ammo = ammo_of(e_weapon);

    if (!dropped)
    {
        LastHit& slot = s_last_hits[victim % LAST_HIT_SLOTS];
        slot.victim = victim;
        slot.attacker = attacker;
        slot.hit_type = hit_type;
        slot.power = power;
        slot.ap = ap;
        slot.time = Device.dwTimeGlobal;
        slot.weapon = e_weapon ? e_weapon->s_name : shared_str();
        slot.ammo = ammo;
    }

    if (g_sv_telemetry < 2)
        return;

    Rec r("hit");
    add_entity(r, "v", e_victim);
    add_entity(r, "k", e_attacker);

    // ⭐⭐A hit on something already dead is NOT a hit on a target, and the ledger
    // has to say so or every question asked of it later is answered wrong.
    //
    // ⚠Measured 28-07-2026: three stalkers, killed with one round each, then
    // shot again while lying there. SVNpcHealth::OnHit correctly ignores those
    // rounds -- a corpse takes no damage -- but they still landed here looking
    // exactly like rounds fired at a living enemy. Anything counting "shots per
    // kill", "hit rate" or "which bone kills" off this file would have been
    // counting corpses, and it would have looked entirely plausible.
    //
    // ⭐Labelled rather than dropped, deliberately: the shot really was fired,
    // and how much ammunition players spend on bodies is itself worth knowing.
    // What must never happen is it being mistaken for something else. Same rule
    // as the "drop" flag beside it -- keep the sample, name what it is.
    if (CSE_ALifeCreatureAbstract* vc = smart_cast<CSE_ALifeCreatureAbstract*>(e_victim))
        if (vc->g_Health() <= 0.f)
            r.B("corpse", true);
    if (e_weapon)
        r.S("wpn", e_weapon->s_name.c_str());
    if (ammo.size())
        r.S("ammo", ammo.c_str());
    r.F("pow", power);
    if (ap > 0.f)
        r.F("ap", ap);
    r.U("ht", hit_type);
    r.U("bone", bone);
    r.B("aim", aim != 0);
    if (dropped)
        r.B("drop", true);
    // The machine-wide clock, so this line can be subtracted from the shooter's
    // own log line in another process, and the impact point in bone space,
    // which is what identifies WHICH shot this was. See shot_latency.h.
    r.U("qpc", CPU::QPC_us());
    {
        string128 ib;
        sprintf_s(ib, "%.6f/%.6f/%.6f", in_bone.x, in_bone.y, in_bone.z);
        r.S("ib", ib);
    }
    if (e_victim)
        r.V("p", e_victim->o_Position);
    r.Emit();
}

void SVTelemetry::OnDeath(CSE_Abstract* victim, CSE_Abstract* killer)
{
    if (!ensure_open() || !victim)
        return;

    Rec r("kill");
    add_entity(r, "v", victim);
    if (killer && killer != victim)
        add_entity(r, "k", killer);
    else
        r.B("self", true);

    // The engine reports a death without a cause; the last hit this entity
    // took is the cause, as long as it is recent enough to still be the
    // same fight.
    const LastHit& slot = s_last_hits[victim->ID % LAST_HIT_SLOTS];
    if (slot.victim == victim->ID && (Device.dwTimeGlobal - slot.time) <= LAST_HIT_TTL_MS)
    {
        if (slot.weapon.size())
            r.S("wpn", slot.weapon.c_str());
        if (slot.ammo.size())
            r.S("ammo", slot.ammo.c_str());
        r.U("ht", slot.hit_type);
        r.F("lastpow", slot.power);
        if (slot.ap > 0.f)
            r.F("ap", slot.ap);
        r.U("hitago", Device.dwTimeGlobal - slot.time);
        if (slot.attacker != 0xffff && (!killer || slot.attacker != killer->ID))
            r.U("hitby", slot.attacker); // hit and kill credit disagree
    }

    r.V("p", victim->o_Position);
    if (killer && killer != victim)
        r.F("dist", victim->o_Position.distance_to(killer->o_Position));
    r.Emit();
}

void SVTelemetry::OnItemMoved(const char* how, CSE_Abstract* item, CSE_Abstract* from, CSE_Abstract* to)
{
    if (!ensure_open() || !item)
        return;

    Rec r("item");
    r.S("how", how);
    r.U("id", item->ID);
    r.S("sect", item->s_name.c_str());
    {
        u32 rounds = 0;
        if (const char* cat = item_category(item, rounds))
        {
            r.S("cat", cat);
            if (rounds)
                r.U("rounds", rounds);
        }
    }
    if (from)
        add_entity(r, "f", from);
    if (to)
        add_entity(r, "t", to);
    r.V("p", item->o_Position);
    r.Emit();
}
