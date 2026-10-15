////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_memory_manager.cpp
//	Created 	: 02.10.2001
//  Modified 	: 19.11.2003
//	Author		: Dmitriy Iassenev
//	Description : Sound memory manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "sound_memory_manager.h"
#include "memory_manager.h"
#include "hit_memory_manager.h"
#include "visual_memory_manager.h"
#include "enemy_manager.h"
#include "danger_manager.h"
#include "memory_space_impl.h"
#include "CustomMonster.h"
#include "ai_object_location.h"
#include "level_graph.h"
#include "sound_user_data_visitor.h"
#include "agent_manager.h"
#include "agent_member_manager.h"
#include "ai/stalker/ai_stalker.h"
#include "ai_threat_map.h"
#include "Actor.h"
#include "actor_defs.h"

#include "client_spawn_manager.h"
#include "memory_manager.h"
#include "../xr_3da/IGame_Persistent.h"
#include "../xr_3da/Environment.h"

const float COMBAT_SOUND_PERCEIVE_RADIUS_SQR = _sqr(5.f);

CSoundMemoryManager::~CSoundMemoryManager()
{
    clear_delayed_objects();
}

void CSoundMemoryManager::Load(LPCSTR section) {}

void CSoundMemoryManager::reinit()
{
    m_sounds = 0;
    m_priorities.clear();
    m_last_sound_time = 0;
    m_sound_threshold = m_min_sound_threshold;
    VERIFY(_valid(m_sound_threshold));
}

void CSoundMemoryManager::reload(LPCSTR section)
{
    m_max_sound_count = READ_IF_EXISTS(pSettings, r_s32, section, "DynamicSoundsCount", 1);
    m_min_sound_threshold = READ_IF_EXISTS(pSettings, r_float, section, "sound_threshold", 0.05f);
    m_self_sound_factor = READ_IF_EXISTS(pSettings, r_float, section, "self_sound_factor", 0.f);
    m_sound_decrease_quant = READ_IF_EXISTS(pSettings, r_u32, section, "self_decrease_quant", 250);
    m_decrease_factor = READ_IF_EXISTS(pSettings, r_float, section, "self_decrease_factor", .95f);
    m_max_hear_dist = READ_IF_EXISTS(pSettings, r_float, section, "max_hear_dist", -1.f);

    LPCSTR sound_perceive_section = READ_IF_EXISTS(pSettings, r_string, section, "sound_perceive_section", section);
    m_weapon_factor = READ_IF_EXISTS(pSettings, r_float, sound_perceive_section, "weapon", 10.f);
    m_item_factor = READ_IF_EXISTS(pSettings, r_float, sound_perceive_section, "item", 1.f);
    m_npc_factor = READ_IF_EXISTS(pSettings, r_float, sound_perceive_section, "npc", 1.f);
    m_anomaly_factor = READ_IF_EXISTS(pSettings, r_float, sound_perceive_section, "anomaly", 1.f);
    m_world_factor = READ_IF_EXISTS(pSettings, r_float, sound_perceive_section, "world", 1.f);
}

IC void CSoundMemoryManager::update_sound_threshold()
{
    VERIFY(_valid(m_self_sound_factor));
    VERIFY(_valid(m_sound_threshold));
    VERIFY(_valid(m_min_sound_threshold));
    VERIFY(!fis_zero(m_decrease_factor));
    VERIFY(m_sound_decrease_quant);
    // t = max(t*f^((tc - tl)/tq),min_threshold)
    m_sound_threshold = _max(m_self_sound_factor * m_sound_threshold * exp(float(Device.dwTimeGlobal - m_last_sound_time) / float(m_sound_decrease_quant) * log(m_decrease_factor)),
                             m_min_sound_threshold);
    VERIFY(_valid(m_sound_threshold));
}

IC u32 CSoundMemoryManager::priority(const MemorySpace::CSoundObject& sound) const
{
    u32 priority = u32(-1);
    xr_map<ESoundTypes, u32>::const_iterator I = m_priorities.begin();
    xr_map<ESoundTypes, u32>::const_iterator E = m_priorities.end();
    for (; I != E; ++I)
        if (((*I).second < priority) && ((*I).first & sound.m_sound_type) == (*I).first)
            priority = (*I).second;
    return (priority);
}

void CSoundMemoryManager::enable(const CObject* object, bool enable)
{
    auto J = std::find(m_sounds->begin(), m_sounds->end(), object_id(object));
    if (J == m_sounds->end())
        return;
    (*J).m_enabled = enable;
}

IC bool is_sound_type(int s, const ESoundTypes& t) { return ((s & t) == t); }

float g_ai_hear_crouch_factor = .5f; // the player's body sounds crouched carry this much of their power

// ⭐How far stalkers hear the PLAYER. 📏[hear] 05/06-10: his running steps were
// heard from 30-32 m by anyone, a camp that had never seen him included -- he
// ran across a field and the squad at the next one turned to meet him, there was
// no creeping up on anybody and no breaking away. The player asked for the
// original's feel: one who has not seen him yet is next to deaf and goes by his
// eyes, and that close; and the ears overall cut by about half.
// The share of a sound's max_ai_distance (xr_area: the falloff stays linear and
// ends sooner, so 0.5 is half the range at any loudness):
// - his steps, things he knocks into, his reload: ai_hear_range for all, times
//   ai_hear_unaware_range for one who has not noticed him (running 32 -> 16 -> ~6.5 m);
// - his gunfire and his rounds landing: ai_hear_shot_range -- a shot is what
//   should alert them, aware or not.
// Monsters keep the stock ears.
float g_ai_hear_range = .5f;
float g_ai_hear_unaware_range = .4f;
float g_ai_hear_shot_range = 1.f;
float g_ai_aware_time = 60.f; // seen him / been hit by him this long ago: still aware

bool CSoundMemoryManager::aware_of_actor(const CObject* actor) const
{
    // in a fight with anybody, or alerted by gunfire, a round, a hit, a grenade (what
    // xr_danger reacts to; not a corpse, and not a step he just heard -- that is
    // an enemy_sound danger, and one step would wake his ears up)
    if (m_stalker->memory().enemy().selected())
        return true;
    if (const CDangerObject* danger = m_stalker->memory().danger().selected())
    {
        switch (danger->type())
        {
        case CDangerObject::eDangerTypeBulletRicochet:
        case CDangerObject::eDangerTypeAttackSound:
        case CDangerObject::eDangerTypeEntityAttacked:
        case CDangerObject::eDangerTypeAttacked:
        case CDangerObject::eDangerTypeGrenade: return true;
        default: break;
        }
    }
    if (m_stalker->agent_manager().member().registered_in_combat(m_stalker))
        return true;

    // seen by him or his squad (the visual memory is the squad's), or hit by him
    const u32 window = u32(g_ai_aware_time * 1000.f);
    const auto recent = [&](const auto& objects) {
        const auto I = std::find(objects.begin(), objects.end(), object_id(actor));
        return I != objects.end() && (*I).m_level_time + window > Device.dwTimeGlobal;
    };
    return recent(m_stalker->memory().visual().objects()) || recent(m_stalker->memory().hit().objects());
}

float CSoundMemoryManager::hear_range(const CObject* who, int sound_type) const
{
    if (!m_stalker || !who || !m_object->g_Alive())
        return 1.f;
    const CObject* actor = smart_cast<const CActor*>(who) ? who : (who->H_Parent() && smart_cast<const CActor*>(who->H_Parent())) ? who->H_Parent() : nullptr;
    if (!actor)
        return 1.f;

    if (is_sound_type(sound_type, SOUND_TYPE_WEAPON_SHOOTING) || is_sound_type(sound_type, SOUND_TYPE_WEAPON_BULLET_HIT))
        return g_ai_hear_shot_range;
    return aware_of_actor(actor) ? g_ai_hear_range : g_ai_hear_range * g_ai_hear_unaware_range;
}

// ⭐Rain and thunder cover the player. In a downpour his steps, his reload, the
// things he knocks into carry 1 - ai_hear_rain_mask of their power at full rain
// (rain_density 1), his shots 1 - ai_hear_rain_mask_shot; for a few seconds
// after a lightning strike -- while the thunder rolls -- far less still. The
// player asked for it: a storm broke out mid-fight on 05-10 and it changed
// nothing for the ears of the stalkers.
float g_ai_hear_rain_mask = .6f;
float g_ai_hear_rain_mask_shot = .25f;

static float weather_mask(int sound_type)
{
    if (!g_pGamePersistent || !g_pGamePersistent->Environment().CurrentEnv)
        return 1.f;
    CEnvironment& env = g_pGamePersistent->Environment();

    static u32 last_bolt = 0;
    if (env.IsThunderboltActive())
        last_bolt = Device.dwTimeGlobal;
    const bool thunder = last_bolt && Device.dwTimeGlobal < last_bolt + 4000;

    const bool shot = is_sound_type(sound_type, SOUND_TYPE_WEAPON_SHOOTING);
    const float rain = clampr(env.CurrentEnv->rain_density, 0.f, 1.f);
    float mask = 1.f - rain * (shot ? g_ai_hear_rain_mask_shot : g_ai_hear_rain_mask);
    if (thunder)
        mask *= shot ? .5f : .2f;
    return mask;
}

// ⭐A sound tells where it sounded, not where its owner stands. fill() puts the
// owner's own position into the memory, and for most sounds that is the same
// place -- footsteps, a shot, a voice. Not for a bullet: the impact is "made"
// by the shooter, so a stalker who heard a round smack the wall next to him
// learned the shooter's exact spot, wherever that was.
// 📏05-10 20:16:58: the player killed Бодя with the silenced VAL from 20 m; no
// one heard the shot, four bandits heard the bullet land (1.6-19.8 m from
// them) and at once all searched around the player -- the 90 s old sweep
// dropped, fresh "sound" information with age 0.
// So when the sound is far from its owner, remember the sound's place: they
// go to look where it landed, not where it came from.
// 📏06-10 13:59: but the landing place is no place for the shooter either. The
// player firing from one spot, his rounds striking next to five villagers
// 10-20 m off, moved "where he is" to each impact in turn -- next to the
// stalker who heard it -- and every watcher re-picked his cover after it:
// a street of men running to and fro. A round landing tells them they are
// under fire, not from where: while they know anything of him at all, the
// place they knew stays; the impact goes in only when they knew nothing.
struct SKnownPlace
{
    bool known{};
    Fvector position{};
    u32 vertex{u32(-1)};
};

// The threat map (ai_threat_map.h) hears a sound of the enemy where it sounded,
// not where his last place in memory is (heard_where_it_sounded): it weighs the
// sound against what else it knows, and only a sound from where he stands --
// a step, a shot -- tells where he is; a round landing does not.
static void threat_map_heard(const CAI_Stalker& listener, const CGameObject& object, const Fvector& position)
{
    const CEntityAlive* enemy = listener.memory().enemy().selected();
    const CObject* owner = object.H_Parent() ? object.H_Parent() : &object;
    if (!enemy || owner->ID() != enemy->ID() || owner->Position().distance_to_sqr(position) > _sqr(3.f))
        return;
    ThreatMap::Heard(enemy->ID(), listener.Position(), position);
}

// taken before the sound's own record is refilled with the owner's position
static SKnownPlace known_place(const CCustomMonster* self, const CGameObject* owner, const Fvector& position)
{
    SKnownPlace result;
    if (!owner || owner->Position().distance_to_sqr(position) <= _sqr(3.f))
        return result;
    const CMemoryInfo info = self->memory().memory(owner);
    if (info.m_object)
    {
        result.known = true;
        result.position = info.m_object_params.m_position;
        result.vertex = info.m_object_params.m_level_vertex_id;
    }
    return result;
}

static void heard_where_it_sounded(SObjectParams& params, const SKnownPlace& known, const CGameObject* owner, const Fvector& position)
{
    if (owner->Position().distance_to_sqr(position) <= _sqr(3.f))
        return;

    if (known.known)
    {
        params.m_position = known.position;
        params.m_level_vertex_id = known.vertex;
        return;
    }

    params.m_position = position;
    if (!ai().get_level_graph())
        return;
    params.m_level_vertex_id = ai().level_graph().vertex_id(position);
    if (!ai().level_graph().valid_vertex_id(params.m_level_vertex_id))
        params.m_level_vertex_id = ai().level_graph().nearest_vertex_id(position);
}

// Instrument for the stealth work: which of the player's sounds gave him away,
// to whom, how loud against the stalker's threshold and from how far. One line
// per stalker and kind of sound a second; with dbg_npc_tactics on.
void CSoundMemoryManager::dbg_heard_actor(CActor& actor, int sound_type, const Fvector& position, float sound_power)
{
    extern int g_dbg_npc_tactics;
    if (!g_dbg_npc_tactics)
        return;

    static xr_map<u32, u32> last_line; // (stalker id << 8 | kind) -> time
    static constexpr std::pair<u32, LPCSTR> kinds[] = {
        {SOUND_TYPE_SHOOTING, "shot"},     {SOUND_TYPE_BULLET_HIT, "bullet_hit"}, {SOUND_TYPE_RECHARGING, "reload"},  {SOUND_TYPE_EMPTY_CLICKING, "empty_click"},
        {SOUND_TYPE_STEP, "step"},         {SOUND_TYPE_TALKING, "voice"},         {SOUND_TYPE_INJURING, "hurt"},     {SOUND_TYPE_OBJECT_COLLIDING, "collide"},
        {SOUND_TYPE_OBJECT_BREAKING, "break"}, {SOUND_TYPE_OBJECT_EXPLODING, "explode"}, {SOUND_TYPE_PICKING_UP, "pick_up"}, {SOUND_TYPE_DROPPING, "drop"},
        {SOUND_TYPE_HIDING, "holster"},    {SOUND_TYPE_TAKING, "draw"},           {SOUND_TYPE_USING, "use"},
    };
    u32 kind = 0;
    LPCSTR name = "other";
    for (u32 i = 0; i < std::size(kinds); ++i)
        if (u32(sound_type) & kinds[i].first)
        {
            kind = i + 1;
            name = kinds[i].second;
            break;
        }

    const u32 key = (u32(m_object->ID()) << 8) | kind;
    u32& last = last_line[key];
    if (Device.dwTimeGlobal < last + 1000)
        return;
    last = Device.dwTimeGlobal;

    const u32 state = actor.MovingState();
    LPCSTR gait = (state & mcSprint) ? "sprint" : !(state & mcAnyMove) ? "still" : (state & mcAccel) ? "walk" : "run";
    Msg("[hear] %u '%s' hears the player: %s (type %08x) power=%.2f threshold=%.2f from %.1f m, player %s%s%s", m_object->ID(), m_object->Name(), name, u32(sound_type),
        sound_power, m_sound_threshold, m_object->Position().distance_to(position), gait, (state & mcCrouch) ? " crouched" : "", aware_of_actor(&actor) ? "" : ", unaware");
}

void CSoundMemoryManager::feel_sound_new(CObject* object, int sound_type, CSound_UserDataPtr user_data, const Fvector& position, float sound_power)
{
    VERIFY(_valid(sound_power));
    if (!m_sounds)
        return;

    if (user_data)
        user_data->accept(m_visitor);

    CObject* self = m_object;
    VERIFY(self);

    // ignore unknown sounds
    if (sound_type == 0xffffffff)
        return;

    CEntityAlive* entity_alive = m_object;
    if (!entity_alive->g_Alive())
        return;

    // ignore distant sounds
    if (m_max_hear_dist >= 0.f)
    {
        Fvector center;
        m_object->Center(center);
        float dist = center.distance_to(position);
        if (dist > m_max_hear_dist)
            return;
    }

    VERIFY(_valid(m_sound_threshold));
    m_object->sound_callback(object, sound_type, position, sound_power);
    VERIFY(_valid(m_sound_threshold));

    update_sound_threshold();

    VERIFY(_valid(sound_power));
    if (is_sound_type(sound_type, SOUND_TYPE_WEAPON))
        sound_power *= m_weapon_factor;

    VERIFY(_valid(sound_power));
    if (is_sound_type(sound_type, SOUND_TYPE_ITEM))
        sound_power *= m_item_factor;

    VERIFY(_valid(sound_power));
    if (is_sound_type(sound_type, SOUND_TYPE_MONSTER))
        sound_power *= m_npc_factor;

    VERIFY(_valid(sound_power));
    if (is_sound_type(sound_type, SOUND_TYPE_ANOMALY))
        sound_power *= m_anomaly_factor;

    VERIFY(_valid(sound_power));
    if (is_sound_type(sound_type, SOUND_TYPE_WORLD))
        sound_power *= m_world_factor;

    // The player creeping: what his body makes -- footsteps, brushing past
    // things -- carries less crouched. Weapon, item and voice sounds do not.
    CActor* actor = smart_cast<CActor*>(object);
    if (actor && (actor->MovingState() & mcCrouch) &&
        !is_sound_type(sound_type, SOUND_TYPE_WEAPON) && !is_sound_type(sound_type, SOUND_TYPE_ITEM) && !(sound_type & SOUND_TYPE_TALKING))
        sound_power *= g_ai_hear_crouch_factor;

    if (actor)
        sound_power *= weather_mask(sound_type);

    VERIFY(_valid(sound_power));
    if (actor && m_stalker && sound_power >= m_sound_threshold)
        dbg_heard_actor(*actor, sound_type, position, sound_power);

    VERIFY(_valid(sound_power));
    if (sound_power >= m_sound_threshold)
    {
        if (is_sound_type(sound_type, SOUND_TYPE_WEAPON_SHOOTING))
        {
            // this is fake!
            CEntityAlive* _entity_alive = smart_cast<CEntityAlive*>(object);
            if (_entity_alive && (self->ID() != _entity_alive->ID()) && (_entity_alive->g_Team() != entity_alive->g_Team()))
                m_object->memory().hit().add(_entity_alive);
        }
        if (!m_stalker || !m_stalker->memory().enemy().selected())
            add(object, sound_type, position, sound_power);
        else
        {
            if (object)
            {
                //				bool		is_shooting = is_sound_type(sound_type,SOUND_TYPE_WEAPON_SHOOTING);
                //				bool		is_colliding = is_sound_type(sound_type,SOUND_TYPE_WORLD_OBJECT_COLLIDING);
                //				bool		very_close = m_stalker->Position().distance_to_sqr(object->Position()) <= COMBAT_SOUND_PERCEIVE_RADIUS_SQR;
                //				if (is_shooting || is_colliding || very_close)
                add(object, sound_type, position, sound_power);
            }
        }
    }

    m_last_sound_time = Device.dwTimeGlobal;
    VERIFY(_valid(m_sound_threshold));
    m_sound_threshold = _max(m_sound_threshold, sound_power);
    VERIFY(_valid(m_sound_threshold));
}

void CSoundMemoryManager::add(CSoundObject& sound_object, bool check_for_existance)
{
    if (check_for_existance)
    {
        if (m_sounds->end() != std::find(m_sounds->begin(), m_sounds->end(), object_id(sound_object.m_object)))
            return;
    }

    VERIFY(m_max_sound_count);
    sound_object.m_first_level_time = Device.dwTimeGlobal;
    if (m_max_sound_count <= m_sounds->size())
    {
        auto I = std::min_element(m_sounds->begin(), m_sounds->end(), SLevelTimePredicate<CGameObject>());
        if (m_sounds->end() != I)
            m_sounds->erase(I);
    }
    m_sounds->push_front(sound_object);
}

void CSoundMemoryManager::add(const CObject* object, int sound_type, const Fvector& position, float sound_power)
{
    // we do not want to save our own sounds
    if (object && (m_object->ID() == object->ID()))
        return;

    // we do not want to save the sounds which was from the items we own
    if (object && object->H_Parent() && (object->H_Parent()->ID() == m_object->ID()))
        return;

    const CGameObject* game_object = smart_cast<const CGameObject*>(object);
    if (!game_object && object)
        return;

    // we do not save sounds from the objects we see (?!)
    if (game_object && m_object->memory().visual().visible_now(game_object))
        return;

    const CGameObject* self = m_object;

    if (m_stalker && game_object)
        threat_map_heard(*m_stalker, *game_object, position);

    const SKnownPlace known = known_place(m_object, game_object, position);

    auto J = std::find(m_sounds->begin(), m_sounds->end(), object_id(object));
    if (m_sounds->end() == J)
    {
        CSoundObject sound_object;

        sound_object.fill(game_object, self, ESoundTypes(sound_type), sound_power, !m_stalker ? squad_mask_type(-1) : m_stalker->agent_manager().member().mask(m_stalker));
        if (!game_object)
            sound_object.m_object_params.m_position = position;
        else
            heard_where_it_sounded(sound_object.m_object_params, known, game_object, position);
        sound_object.m_first_level_time = Device.dwTimeGlobal;
        add(sound_object);
    }
    else
    {
        (*J).fill(game_object, self, ESoundTypes(sound_type), sound_power,
                  (!m_stalker ? (*J).m_squad_mask.get() : ((*J).m_squad_mask.get() | m_stalker->agent_manager().member().mask(m_stalker))));
        if (!game_object)
            (*J).m_object_params.m_position = position;
        else
            heard_where_it_sounded((*J).m_object_params, known, game_object, position);
    }
}

struct CRemoveOfflinePredicate
{
    bool operator()(const CSoundObject& object) const
    {
        return (!object.m_object || !!object.m_object->getDestroy() || object.m_object->H_Parent());
    }
};

void CSoundMemoryManager::update()
{
    START_PROFILE("Memory Manager/sounds::update")

    clear_delayed_objects();

    VERIFY(m_sounds);
    m_sounds->erase(std::remove_if(m_sounds->begin(), m_sounds->end(), CRemoveOfflinePredicate()), m_sounds->end());

    STOP_PROFILE
}

struct CSoundObjectPredicate
{
    const CObject* m_object;

    CSoundObjectPredicate(const CObject* object) : m_object(object) {}

    bool operator()(const MemorySpace::CSoundObject& sound_object) const
    {
        if (!m_object)
            return (!sound_object.m_object);

        if (!sound_object.m_object)
            return (false);

        return (m_object->ID() == sound_object.m_object->ID());
    }
};

void CSoundMemoryManager::remove_links(CObject* object)
{
    VERIFY(m_sounds);
    SOUNDS::iterator I = std::find_if(m_sounds->begin(), m_sounds->end(), CSoundObjectPredicate(object));
    if (I != m_sounds->end())
        m_sounds->erase(I);
}

void CSoundMemoryManager::save(NET_Packet& packet) const
{
    if (!m_object->g_Alive())
        return;

    packet.w_u8((u8)objects().size());

    SOUNDS::const_iterator I = objects().begin();
    SOUNDS::const_iterator E = objects().end();
    for (; I != E; ++I)
    {
        VERIFY((*I).m_object);
        packet.w_u16((*I).m_object->ID());
        // object params
        packet.w_u32((*I).m_object_params.m_level_vertex_id);
        packet.w_vec3((*I).m_object_params.m_position);
       // self params
        packet.w_u32((*I).m_self_params.m_level_vertex_id);
        packet.w_vec3((*I).m_self_params.m_position);
        packet.w_u32((Device.dwTimeGlobal >= (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_level_time) : 0);
        packet.w_u32((Device.dwTimeGlobal >= (*I).m_level_time) ? (Device.dwTimeGlobal - (*I).m_last_level_time) : 0);
        packet.w_u32((*I).m_sound_type);
        packet.w_float((*I).m_power);
    }
}

void CSoundMemoryManager::load(IReader& packet)
{
    if (!m_object->g_Alive())
        return;

    auto callback = fastdelegate::MakeDelegate(&m_object->memory(), &CMemoryManager::on_requested_spawn);

    int count = packet.r_u8();
    for (int i = 0; i < count; ++i)
    {
        CDelayedSoundObject delayed_object;
        delayed_object.m_object_id = packet.r_u16();

        CSoundObject& object = delayed_object.m_sound_object;
        object.m_object = smart_cast<CGameObject*>(Level().Objects.net_Find(delayed_object.m_object_id));
        // object params
        object.m_object_params.m_level_vertex_id = packet.r_u32();
        packet.r_fvector3(object.m_object_params.m_position);
        // self params
        object.m_self_params.m_level_vertex_id = packet.r_u32();
        packet.r_fvector3(object.m_self_params.m_position);
        VERIFY(Device.dwTimeGlobal >= object.m_level_time);
        object.m_level_time = packet.r_u32();
        object.m_level_time = Device.dwTimeGlobal >= object.m_level_time ? Device.dwTimeGlobal - object.m_level_time : 0;
        object.m_first_level_time = Device.dwTimeGlobal;
        VERIFY(Device.dwTimeGlobal >= object.m_last_level_time);
        object.m_last_level_time = packet.r_u32();
        object.m_last_level_time = Device.dwTimeGlobal >= object.m_last_level_time ? Device.dwTimeGlobal - object.m_last_level_time : 0;
        object.m_sound_type = (ESoundTypes)packet.r_u32();
        object.m_power = packet.r_float();

        if (object.m_object)
        {
            add(object, true);
            continue;
        }

        m_delayed_objects.push_back(delayed_object);

        const CClientSpawnManager::CSpawnCallback* spawn_callback = Level().client_spawn_manager().callback(delayed_object.m_object_id, m_object->ID());
        if (!spawn_callback || !spawn_callback->m_object_callback)
            Level().client_spawn_manager().add(delayed_object.m_object_id, m_object->ID(), callback);
    }
}

void CSoundMemoryManager::clear_delayed_objects()
{
    if (m_delayed_objects.empty())
        return;

    CClientSpawnManager& manager = Level().client_spawn_manager();
    DELAYED_SOUND_OBJECTS::const_iterator I = m_delayed_objects.begin();
    DELAYED_SOUND_OBJECTS::const_iterator E = m_delayed_objects.end();
    for (; I != E; ++I)
        if (manager.callback((*I).m_object_id, m_object->ID()))
            manager.remove((*I).m_object_id, m_object->ID());

    m_delayed_objects.clear();
}

void CSoundMemoryManager::on_requested_spawn(CObject* object)
{
    DELAYED_SOUND_OBJECTS::iterator I = m_delayed_objects.begin();
    DELAYED_SOUND_OBJECTS::iterator E = m_delayed_objects.end();
    for (; I != E; ++I)
    {
        if ((*I).m_object_id != object->ID())
            continue;

        if (m_object->g_Alive())
        {
            (*I).m_sound_object.m_object = smart_cast<CGameObject*>(object);
            VERIFY((*I).m_sound_object.m_object);
            add((*I).m_sound_object, true);
        }

        m_delayed_objects.erase(I);
        return;
    }
}

// Out of line: the lambda needs the complete object type, which the inline header does not have.
const CSoundMemoryManager::SOUNDS& CSoundMemoryManager::objects() const
{
    m_sounds->erase(std::remove_if(m_sounds->begin(), m_sounds->end(), [](const auto& obj) { return obj.m_object->getDestroy(); }), m_sounds->end());
    return *m_sounds;
}
