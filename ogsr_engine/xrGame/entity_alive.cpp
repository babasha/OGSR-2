#include "StdAfx.h"
#include "../xrCore/load_prof.h"   // per-spawn breakdown
#include "entity_alive.h"
#include "InventoryOwner.h"
#include "Inventory.h"
#include "PhysicsShell.h"
#include "../xr_3da/GameMtlLib.h"
#include "PHMovementControl.h"
#include "Wound.h"
#include "xrMessages.h"
#include "Level.h"
#include "../Include/xrRender/Kinematics.h"
#include "relation_registry.h"
#include "monster_community.h"
#include "EntityCondition.h"
#include "script_game_object.h"
#include "Hit.h"
#include "PHDestroyable.h"
#include "CharacterPhysicsSupport.h"
#include "script_callback_ex.h"
#include "game_object_space.h"
#include "material_manager.h"
#include "game_base_space.h"
#include "xrServer.h"
#include "sv_npc_health.h"
#include "CustomMonster.h" // net_note_flinch -- a hit as a held wire flag
#include "Actor.h"
// Putting a revived body back into its group -- see OnServerRevive. The squad
// memories hold POINTERS handed out by this hierarchy, so re-registration is
// what makes a revived creature safe to think.
#include "seniority_hierarchy_holder.h"
#include "team_hierarchy_holder.h"
#include "squad_hierarchy_holder.h"
#include "group_hierarchy_holder.h"

#define SMALL_ENTITY_RADIUS 0.6f
#define BLOOD_MARKS_SECT "bloody_marks"

//отметки крови на стенах
FactoryPtr<IWallMarkArray>* CEntityAlive::m_pBloodMarksVector = nullptr;
float CEntityAlive::m_fBloodMarkSizeMin = 0.f;
float CEntityAlive::m_fBloodMarkSizeMax = 0.f;
float CEntityAlive::m_fBloodMarkDistance = 0.f;
float CEntityAlive::m_fNominalHit = 0.f;

//капание крови
FactoryPtr<IWallMarkArray>* CEntityAlive::m_pBloodDropsVector = nullptr;
float CEntityAlive::m_fStartBloodWoundSize = 0.3f;
float CEntityAlive::m_fStopBloodWoundSize = 0.1f;
float CEntityAlive::m_fBloodDropSize = 0.03f;

//минимальный размер ожега, после которого горят партиклы
//минимальное время горения
u32 CEntityAlive::m_dwMinBurnTime = 10000;
//размер раны, чтоб запустить партиклы
float CEntityAlive::m_fStartBurnWoundSize = 0.3f;
//размер раны, чтоб остановить партиклы
float CEntityAlive::m_fStopBurnWoundSize = 0.1f;

STR_VECTOR* CEntityAlive::m_pFireParticlesVector = NULL;

/////////////////////////////////////////////
// CEntityAlive
/////////////////////////////////////////////
CEntityAlive::CEntityAlive()
{
    monster_community = xr_new<MONSTER_COMMUNITY>();

    m_ef_weapon_type = u32(-1);
    m_ef_detector_type = u32(-1);
    b_eating = false;
    m_use_timeout = 5000;
    m_used_time = Device.dwTimeGlobal;
    m_squad_index = u8(-1);

    m_material_manager = 0;
}

CEntityAlive::~CEntityAlive()
{
    xr_delete(monster_community);
    xr_delete(m_material_manager);
}

void CEntityAlive::Load(LPCSTR section)
{
    // The actor's own Load is 75 ms of the client spawn burst and all of it lands
    // in here. Per-part QPC, logged only when the total is worth a line (a monster
    // spawning mid-fight must not spam the log).
    const double _k = 1000.0 / double(CPU::QPCFreq());
    u64 _t = CPU::QPC();
    auto _take = [&_t]() -> double { const u64 n = CPU::QPC(); const double d = double(n - _t); _t = n; return d; };

    CEntity::Load(section);                                                                 const double _a = _take();
    conditions().LoadCondition(section);                                                    const double _b = _take();
    conditions().LoadImmunities(pSettings->r_string(section, "immunities_sect"), pSettings); const double _c = _take();

    m_fFood = 100 * pSettings->r_float(section, "ph_mass");

    // bloody wallmarks
    if (0 == m_pBloodMarksVector)
        LoadBloodyWallmarks(BLOOD_MARKS_SECT);
    const double _d = _take();

    if (0 == m_pFireParticlesVector)
        LoadFireParticles("entity_fire_particles");
    const double _e = _take();

    //биолог. вид к торому принадлежит монстр или персонаж
    monster_community->set(pSettings->r_string(section, "species"));
    const double _f = _take();

    if (_k * (_a + _b + _c + _d + _e + _f) >= 5.0)
        Msg("[load step]   CEntityAlive::Load '%s': entity %.0f | condition %.0f | immunities %.0f | "
            "blood wallmarks %.0f | fire particles %.0f | species %.0f ms",
            section, _k * _a, _k * _b, _k * _c, _k * _d, _k * _e, _k * _f);
}

void CEntityAlive::LoadBloodyWallmarks(LPCSTR section)
{
    VERIFY(0 == m_pBloodMarksVector);
    VERIFY(0 == m_pBloodDropsVector);
    m_pBloodMarksVector = xr_new<FactoryPtr<IWallMarkArray>>();
    m_pBloodDropsVector = xr_new<FactoryPtr<IWallMarkArray>>();

    //кровавые отметки на стенах
    string256 tmp;
    LPCSTR wallmarks_name = pSettings->r_string(section, "wallmarks");

    int cnt = _GetItemCount(wallmarks_name);

    for (int k = 0; k < cnt; ++k)
        (*m_pBloodMarksVector)->AppendMark(_GetItem(wallmarks_name, k, tmp));

    m_fBloodMarkSizeMin = pSettings->r_float(section, "min_size");
    m_fBloodMarkSizeMax = pSettings->r_float(section, "max_size");
    m_fBloodMarkDistance = pSettings->r_float(section, "dist");
    m_fNominalHit = pSettings->r_float(section, "nominal_hit");

    //капли крови с открытых ран
    wallmarks_name = pSettings->r_string(section, "blood_drops");
    cnt = _GetItemCount(wallmarks_name);

    for (int k = 0; k < cnt; ++k)
        (*m_pBloodDropsVector)->AppendMark(_GetItem(wallmarks_name, k, tmp));

    m_fStartBloodWoundSize = pSettings->r_float(section, "start_blood_size");
    m_fStopBloodWoundSize = pSettings->r_float(section, "stop_blood_size");
    m_fBloodDropSize = pSettings->r_float(section, "blood_drop_size");
}

void CEntityAlive::UnloadBloodyWallmarks()
{
    if (m_pBloodMarksVector)
    {
        // m_pBloodMarksVector->clear	();
        xr_delete(m_pBloodMarksVector);
    }
    if (m_pBloodDropsVector)
    {
        // m_pBloodDropsVector->clear	();
        xr_delete(m_pBloodDropsVector);
    }
}

void CEntityAlive::LoadFireParticles(LPCSTR section)
{
    m_pFireParticlesVector = xr_new<STR_VECTOR>();

    string256 tmp;
    LPCSTR particles_name = pSettings->r_string(section, "fire_particles");

    int cnt = _GetItemCount(particles_name);

    shared_str s;
    for (int k = 0; k < cnt; ++k)
    {
        s = _GetItem(particles_name, k, tmp);
        m_pFireParticlesVector->push_back(s);
    }

    m_fStartBurnWoundSize = pSettings->r_float(section, "start_burn_size");
    m_fStopBurnWoundSize = pSettings->r_float(section, "stop_burn_size");

    m_dwMinBurnTime = pSettings->r_u32(section, "min_burn_time");
}

void CEntityAlive::UnloadFireParticles()
{
    if (m_pFireParticlesVector)
    {
        m_pFireParticlesVector->clear();
        xr_delete(m_pFireParticlesVector);
    }
}

void CEntityAlive::reinit()
{
    CEntity::reinit();

    m_fAccuracy = 25.f;
    m_fIntelligence = 25.f;
}

void CEntityAlive::reload(LPCSTR section)
{
    CEntity::reload(section);
    //	CEntityCondition::reload(section);

    m_ef_creature_type = pSettings->r_u32(section, "ef_creature_type");
    m_ef_weapon_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_weapon_type", u32(-1));
    m_ef_detector_type = READ_IF_EXISTS(pSettings, r_u32, section, "ef_detector_type", u32(-1));

    m_fFood = 100 * pSettings->r_float(section, "ph_mass");
}

void CEntityAlive::shedule_Update(u32 dt)
{
    inherited::shedule_Update(dt);

    // condition update with the game time pass
    conditions().UpdateConditionTime();
    conditions().UpdateCondition();
    //Обновление партиклов огня
    UpdateFireParticles();
    //капли крови
    UpdateBloodDrops();
    //обновить раны
    conditions().UpdateWounds();

    //убить сущность
    if (Local() && !g_Alive() && !AlreadyDie())
    {
        if (conditions().GetWhoHitLastTime())
        {
            //			Msg			("%6d : KillEntity from CEntityAlive (using who hit last time) for object %s",Device.dwTimeGlobal,*cName());
            KillEntity(conditions().GetWhoHitLastTimeID());
        }
        else
        {
            //			Msg			("%6d : KillEntity from CEntityAlive for object %s",Device.dwTimeGlobal,*cName());
            KillEntity(ID());
        }
    }
}

BOOL CEntityAlive::net_Spawn(CSE_Abstract* DC)
{
    LoadProf::Scope _lpNetSpawn("entityalive.net_Spawn");
    //установить команду в соответствии с community
    /*	if(monster_community->team() != 255)
            id_Team = monster_community->team();*/

    conditions().reinit();
    inherited::net_Spawn(DC);

    m_BloodWounds.clear();
    m_ParticleWounds.clear();

    //добавить кровь и огонь на партиклы, если нужно
    for (WOUND_VECTOR::const_iterator it = conditions().wounds().begin(); conditions().wounds().end() != it; ++it)
    {
        CWound* pWound = *it;
        StartFireParticles(pWound);
        StartBloodDrops(pWound);
    }

    return (TRUE);
}

void CEntityAlive::net_Destroy() { inherited::net_Destroy(); }

float g_ai_head_scale_vs_player = 1.f; // an NPC bullet in the player's head: bone factor at most this; 0 = actor.ltx as is

void CEntityAlive::Hit(SHit* pHDS)
{
    SHit HDS = *pHDS;

    //-------------------------------------------------------------------
    if (HDS.hit_type == ALife::eHitTypeWound_2)
        HDS.hit_type = ALife::eHitTypeWound;
    //-------------------------------------------------------------------
    CDamageManager::HitScale(HDS.boneID, conditions().hit_bone_scale(), conditions().wound_bone_scale(), HDS.aim_bullet);

    // ⭐An NPC's bullet in the PLAYER's head counts no more than a body hit.
    // actor.ltx raised the player's head from 0.2 to 2.5 for multiplayer, so a
    // headshot between players kills; but it also made every bandit who
    // clipped the player's head do 12.5x what the game was tuned for -- one
    // bullet from 44% health in the log. Player against player keeps 2.5.
    extern float g_ai_head_scale_vs_player;
    if (HDS.hit_type == ALife::eHitTypeFireWound && g_ai_head_scale_vs_player > 0.f && smart_cast<CActor*>(this) && smart_cast<CCustomMonster*>(HDS.who) &&
        conditions().hit_bone_scale() > g_ai_head_scale_vs_player)
        conditions().hit_bone_scale() = g_ai_head_scale_vs_player;

    // The health this copy is about to spend, and the bone factor it spends it
    // through. Sampled unconditionally on the host: for a creature the server
    // simulates this is the ONLY place the real cost of a hit exists, and the
    // record below is built from it. dbg_death only decides whether it is also
    // printed.
    extern int g_dbg_death;
    const float hp_before = GetfHealth();

    //изменить состояние, перед тем как родительский класс обработает хит
    CWound* pWound = conditions().ConditionHit(&HDS);

    if (pWound)
    {
        if (ALife::eHitTypeBurn == HDS.hit_type)
            StartFireParticles(pWound);
        else if (ALife::eHitTypeWound == HDS.hit_type || ALife::eHitTypeFireWound == HDS.hit_type)
            StartBloodDrops(pWound);
    }

    if (HDS.hit_type != ALife::eHitTypeTelepatic)
    {
        //добавить кровь на стены
        if (!use_simplified_visual())
            BloodyWallmarks(HDS.damage(), HDS.dir, HDS.bone(), HDS.p_in_bone_space);
    }

    //-------------------------------------------
    conditions().SetConditionDeltaTime(0);
    //-------------------------------------------
    inherited::Hit(&HDS);

    if (g_dbg_death)
        Msg("~ [hp] applied id=%u '%s' bone=%u pow=%.4f bscale=%.3f wscale=%.3f hp %.4f -> %.4f local=%d", ID(), cName().c_str(), HDS.boneID, HDS.power,
            conditions().hit_bone_scale(), conditions().wound_bone_scale(), hp_before, GetfHealth(), Local() ? 1 : 0);

    // ⭐The server's record of a creature's health, written where the health is
    // actually spent rather than recomputed beside it. Only on the host (a
    // client has no Server), and only for a copy this process simulates -- a
    // remote client's mirror of the same creature runs this code too, and its
    // arithmetic is a prediction, not a fact.
    if (Local() && Level().Server)
    {
        // ⚠`pHDS->power` is NOT the number that crossed the wire: by the time a
        // stalker's hit reaches here, CAI_Stalker::Hit has already handed down
        // its own modified copy. SHit::full_power is the untouched one --
        // Read_Packet_Cont stamps it straight off the packet and nothing in the
        // damage chain writes it again. Recording the wrong one would have made
        // the mod's multiplier invisible in exactly the record built to show it.
        const float wire = (pHDS->full_power > 0.f) ? pHDS->full_power : pHDS->power;
        SVNpcHealth::NoteApplied(u16(ID()), HDS.whoID, HDS.boneID, u16(HDS.hit_type), wire, HDS.power, hp_before, GetfHealth());

        // ...and the same event as PRESENTATION: a man who has just been shot
        // reacts, and until now the clients had no way to know it happened
        // between two snapshots. Latched into the intent block, alongside the
        // shot flag. Same gate as the record above -- only the copy that
        // actually spends the health may report it.
        if (CCustomMonster* hurt = smart_cast<CCustomMonster*>(this))
            hurt->net_note_flinch();
    }

    if (g_Alive())
    {
        CEntityAlive* EA = smart_cast<CEntityAlive*>(HDS.who);
        if (EA && EA->g_Alive() && EA->ID() != ID())
        {
            RELATION_REGISTRY().FightRegister(EA->ID(), ID(), this->tfGetRelationType(EA), HDS.damage());
            RELATION_REGISTRY().Action(EA, this, RELATION_REGISTRY::ATTACK);
        }
    }
}

void CEntityAlive::OnServerRevive(float health_fraction)
{
    if (g_Alive())
    {
        Msg("~ [revive] %u '%s' was already alive -- nothing to undo", ID(), cName().c_str());
        return;
    }

    // 1. The condition. Wounds go first: bleeding that outlived the body would
    //    take the restored health straight back down, and a man who is revived
    //    only to die again in four seconds reads as the revive not working.
    conditions().ClearWounds();
    SetfHealth(GetMaxHealth() * health_fraction);

    // 2. The entity's own record of having died. ⚠This is what AlreadyDie()
    //    answers, and half the engine asks it rather than asking health --
    //    CCustomMonster::visual_name swaps in the corpse model off it, and the
    //    corpse-removal timer runs off m_game_death_time. Left set, the body
    //    walks around and is still scheduled to be cleaned up as a corpse.
    m_level_death_time = 0;
    m_game_death_time = 0;

    // 3. The physics. A corpse is a ragdoll shell; a living creature is a
    //    capsule. ⚠Order matters: the shell has to go before the capsule is
    //    made, or two physics bodies claim the same space and the character
    //    controller is created inside its own corpse.
    if (CPhysicsShell* shell = PPhysicsShell())
    {
        shell->Deactivate();
        xr_delete(shell);
        PPhysicsShell() = nullptr;
    }

    if (CCharacterPhysicsSupport* cps = character_physics_support())
    {
        cps->SetState(CCharacterPhysicsSupport::esAlive);
        cps->CreateCharacter();
    }

    // Death took this out of the spatial types that make it react to sound
    // (CEntityAlive::Die); a living thing that cannot hear is not revived.
    if (ISpatial* self = smart_cast<ISpatial*>(this))
        self->spatial.type |= STYPE_REACTTOSOUND;

    // 4. ⭐⭐⭐Back into its GROUP -- and this one is not bookkeeping, it is the
    //    difference between a revived creature and a crash. Measured
    //    05-08-2026: without it the host faulted every frame inside
    //    CVisualMemoryManager::objects() as soon as the revived stalker began
    //    to think.
    //
    //    ⚠The chain is not obvious from the crash. CEntity::Die unregisters the
    //    member; CGroupHierarchyHolder::unregister_in_group_senses then does
    //    set_squad_objects(0) on the visual, sound and hit memories -- and
    //    those hold a POINTER to the squad's shared list, with objects()
    //    dereferencing it unchecked. A dead creature never asks, so the null
    //    sits there harmlessly until something makes it alive again and its
    //    combat planner calls visible_now().
    //
    //    ⭐So the fix is not a null check bolted onto the accessor: it is
    //    performing the exact inverse of what Die did. Registration puts the
    //    pointers back through the engine's own path.
    re_register_member();

    Msg("~ [revive] %u '%s' is up again at %.3f/%.3f health", ID(), cName().c_str(), GetfHealth(), GetMaxHealth());
}

void CEntityAlive::Die(CObject* who)
{
    RELATION_REGISTRY().Action(smart_cast<CEntityAlive*>(who), this, RELATION_REGISTRY::KILL);
    inherited::Die(who);

    const CGameObject* who_object = smart_cast<const CGameObject*>(who);
    callback(GameObject::eDeath)(lua_game_object(), who_object ? who_object->lua_game_object() : 0);

    if (!getDestroy())
    {
        NET_Packet P;
        u_EventGen(P, GE_ASSIGN_KILLER, ID());
        P.w_u16(who ? who->ID() : u16(-1));
        u_EventSend(P);
    }

    // disable react to sound
    ISpatial* self = smart_cast<ISpatial*>(this);
    if (self)
        self->spatial.type &= ~STYPE_REACTTOSOUND;
}

//вывзывает при подсчете хита
float CEntityAlive::CalcCondition(float /**hit/**/)
{
    conditions().UpdateCondition();

    // dont call inherited::CalcCondition it will be meaningless
    return conditions().GetHealthLost(); //*100.f;
}

///////////////////////////////////////////////////////////////////////
u16 CEntityAlive::PHGetSyncItemsNumber()
{
    if (character_physics_support()->movement()->CharacterExist())
        return 1;
    else
        return inherited::PHGetSyncItemsNumber();
}
CPHSynchronize* CEntityAlive::PHGetSyncItem(u16 item)
{
    if (character_physics_support()->movement()->CharacterExist())
        return character_physics_support()->movement()->GetSyncItem();
    else
        return inherited::PHGetSyncItem(item);
}
void CEntityAlive::PHUnFreeze()
{
    if (character_physics_support()->movement()->CharacterExist())
        character_physics_support()->movement()->UnFreeze();
    else if (m_pPhysicsShell)
        m_pPhysicsShell->UnFreeze();
}
void CEntityAlive::PHFreeze()
{
    if (character_physics_support()->movement()->CharacterExist())
        character_physics_support()->movement()->Freeze();
    else if (m_pPhysicsShell)
        m_pPhysicsShell->Freeze();
}
//////////////////////////////////////////////////////////////////////

//добавление кровавых отметок на стенах, после получения хита
void CEntityAlive::BloodyWallmarks(float P, const Fvector& dir, s16 element, const Fvector& position_in_object_space)
{
    if (BI_NONE == (u16)element)
        return;

    //вычислить координаты попадания
    IKinematics* V = smart_cast<IKinematics*>(Visual());

    Fvector start_pos = position_in_object_space;
    if (V)
    {
        Fmatrix& m_bone = (V->LL_GetBoneInstance(u16(element))).mTransform;
        m_bone.transform_tiny(start_pos);
    }
    XFORM().transform_tiny(start_pos);

    float small_entity = 1.f;
    if (Radius() < SMALL_ENTITY_RADIUS)
        small_entity = 0.5;

    float wallmark_size = m_fBloodMarkSizeMax;
    wallmark_size *= (P / m_fNominalHit);
    wallmark_size *= small_entity;
    clamp(wallmark_size, m_fBloodMarkSizeMin, m_fBloodMarkSizeMax);

    VERIFY(m_pBloodMarksVector);
    PlaceBloodWallmark(dir, start_pos, m_fBloodMarkDistance, wallmark_size, &**m_pBloodMarksVector);
}

void CEntityAlive::PlaceBloodWallmark(const Fvector& dir, const Fvector& start_pos, float trace_dist, float wallmark_size, IWallMarkArray* pwallmarks_vector)
{
    collide::rq_result result;

    if (!Level().ObjectSpace.RayPick(start_pos, dir, trace_dist, collide::rqtBoth, result, this))
        return;

    //вычислить точку попадания
    Fvector end_point;
    end_point.set(0, 0, 0);
    end_point.mad(start_pos, dir, result.range);

    if (result.O)
    { // Dynamic object
        /*
                const auto pK = smart_cast<IKinematics*>(result.O->Visual());
                if (!pK)
                    return;

                const auto& bone_data = pK->LL_GetData((u16)result.element);
                auto pMaterial = GMLib.GetMaterialByIdx(bone_data.game_mtl_idx);

                if (pMaterial->Flags.is(SGameMtl::flBloodmark))
                    ::Render->add_SkeletonWallmark(&result.O->renderable.xform, pK, pwallmarks_vector, end_point, dir, wallmark_size);
        */
    }
    else
    { //если кровь долетела до статического объекта
        auto pTri = Level().ObjectSpace.GetStaticTris() + result.element;
        auto pMaterial = GMLib.GetMaterialByIdx(pTri->material);

        if (pMaterial->Flags.is(SGameMtl::flBloodmark))
        {
            //вычислить нормаль к пораженной поверхности
            auto pVerts = Level().ObjectSpace.GetStaticVerts();

            //добавить отметку на материале
            ::Render->add_StaticWallmark(pwallmarks_vector, end_point, wallmark_size, pTri, pVerts);
        }
    }
}

void CEntityAlive::StartFireParticles(CWound* pWound)
{
    if (pWound->TypeSize(ALife::eHitTypeBurn) > m_fStartBurnWoundSize)
    {
        if (std::find(m_ParticleWounds.begin(), m_ParticleWounds.end(), pWound) == m_ParticleWounds.end())
        {
            m_ParticleWounds.push_back(pWound);
        }

        IKinematics* V = smart_cast<IKinematics*>(Visual());

        u16 particle_bone = CParticlesPlayer::GetNearestBone(V, pWound->GetBoneNum());
        VERIFY(particle_bone < 64 || BI_NONE == particle_bone);

        pWound->SetParticleBoneNum(particle_bone);
        pWound->SetParticleName((*m_pFireParticlesVector)[::Random.randI(0, m_pFireParticlesVector->size())]);

        if (BI_NONE != particle_bone)
        {
            CParticlesPlayer::StartParticles(pWound->GetParticleName(), pWound->GetParticleBoneNum(), Fvector().set(0, 1, 0), ID(),
                                             u32(float(m_dwMinBurnTime) * ::Random.randF(0.5f, 1.5f)), false);
        }
        else
        {
            CParticlesPlayer::StartParticles(pWound->GetParticleName(), Fvector().set(0, 1, 0), ID(), u32(float(m_dwMinBurnTime) * ::Random.randF(0.5f, 1.5f)), false);
        }
    }
}

void CEntityAlive::UpdateFireParticles()
{
    if (m_ParticleWounds.empty())
        return;

    //	WOUND_VECTOR_IT last_it;

    for (WOUND_VECTOR_IT it = m_ParticleWounds.begin(); it != m_ParticleWounds.end();)
    {
        CWound* pWound = *it;
        float burn_size = pWound->TypeSize(ALife::eHitTypeBurn);

        if (pWound->GetDestroy() || (burn_size > 0 && (burn_size < m_fStopBurnWoundSize || !g_Alive())))
        {
            CParticlesPlayer::AutoStopParticles(pWound->GetParticleName(), pWound->GetParticleBoneNum(), u32(float(m_dwMinBurnTime) * ::Random.randF(0.5f, 1.5f)));
            it = m_ParticleWounds.erase(it);
            continue;
        }
        ++it;
    }
}

ALife::ERelationType CEntityAlive::tfGetRelationType(const CEntityAlive* tpEntityAlive) const
{
    int relation = MONSTER_COMMUNITY::relation(this->monster_community->index(), tpEntityAlive->monster_community->index());

    switch (relation)
    {
    case 1: return (ALife::eRelationTypeFriend); break;
    case 0: return (ALife::eRelationTypeNeutral); break;
    case -1: return (ALife::eRelationTypeEnemy); break;
    case -2: return (ALife::eRelationTypeWorstEnemy); break;

    default: return (ALife::eRelationTypeDummy); break;
    }
};

bool CEntityAlive::is_relation_enemy(const CEntityAlive* tpEntityAlive) const
{
    return ((tfGetRelationType(tpEntityAlive) == ALife::eRelationTypeEnemy) || (tfGetRelationType(tpEntityAlive) == ALife::eRelationTypeWorstEnemy));
}

void CEntityAlive::StartBloodDrops(CWound* pWound)
{
    if (pWound->BloodSize() > m_fStartBloodWoundSize)
    {
        if (std::find(m_BloodWounds.begin(), m_BloodWounds.end(), pWound) == m_BloodWounds.end())
        {
            m_BloodWounds.push_back(pWound);
            pWound->m_fDropTime = 0.f;
        }
    }
}

void CEntityAlive::UpdateBloodDrops()
{
    static float m_fBloodDropTimeMax = pSettings->r_float(BLOOD_MARKS_SECT, "blood_drop_time_max");
    static float m_fBloodDropTimeMin = pSettings->r_float(BLOOD_MARKS_SECT, "blood_drop_time_min");

    if (m_BloodWounds.empty())
        return;

    if (!g_Alive())
    {
        m_BloodWounds.clear();
        return;
    }

    //	WOUND_VECTOR_IT last_it;

    for (WOUND_VECTOR_IT it = m_BloodWounds.begin(); it != m_BloodWounds.end();)
    {
        CWound* pWound = *it;
        float blood_size = pWound->BloodSize();

        if (pWound->GetDestroy() || blood_size < m_fStopBloodWoundSize)
        {
            it = m_BloodWounds.erase(it);
            continue;
        }

        if (pWound->m_fDropTime < Device.fTimeGlobal)
        {
            float size_k = blood_size - m_fStopBloodWoundSize;
            size_k = size_k < 1.f ? size_k : 1.f;
            pWound->m_fDropTime = Device.fTimeGlobal + (m_fBloodDropTimeMax - (m_fBloodDropTimeMax - m_fBloodDropTimeMin) * size_k) * Random.randF(0.8f, 1.2f);
            VERIFY(m_pBloodDropsVector);
            if (pWound->GetBoneNum() != BI_NONE)
            {
                Fvector pos;
                Fvector pos_distort;
                pos_distort.random_dir();
                pos_distort.mul(0.15f);
                CParticlesPlayer::GetBonePos(this, pWound->GetBoneNum(), Fvector().set(0, 0, 0), pos);
                pos.add(pos_distort);
                PlaceBloodWallmark(Fvector().set(0.f, -1.f, 0.f), pos, m_fBloodMarkDistance, m_fBloodDropSize, &**m_pBloodDropsVector);
            }
        }
        ++it;
    }
}

void CEntityAlive::save(NET_Packet& output_packet)
{
    inherited::save(output_packet);
    conditions().save(output_packet);
}

void CEntityAlive::load(IReader& input_packet)
{
    inherited::load(input_packet);
    conditions().load(input_packet);
}

BOOL CEntityAlive::net_SaveRelevant() { return (TRUE); }

CEntityConditionSimple* CEntityAlive::create_entity_condition(CEntityConditionSimple* ec)
{
    if (!ec)
        m_entity_condition = xr_new<CEntityCondition>(this);
    else
        m_entity_condition = smart_cast<CEntityCondition*>(ec);

    return (inherited::create_entity_condition(m_entity_condition));
}

float CEntityAlive::SetfRadiation(float value)
{
    conditions().radiation() = value / 100.f;
    return value;
}
float CEntityAlive::g_Radiation() const { return conditions().GetRadiation() * 100.f; }

DLL_Pure* CEntityAlive::_construct()
{
    inherited::_construct();
    if (character_physics_support())
        m_material_manager = xr_new<CMaterialManager>(this, character_physics_support()->movement());
    return (this);
}

u32 CEntityAlive::ef_creature_type() const { return (m_ef_creature_type); }

u32 CEntityAlive::ef_weapon_type() const
{
    VERIFY(m_ef_weapon_type != u32(-1));
    return (m_ef_weapon_type);
}

u32 CEntityAlive::ef_detector_type() const
{
    VERIFY(m_ef_detector_type != u32(-1));
    return (m_ef_detector_type);
}
void CEntityAlive::PHGetLinearVell(Fvector& velocity)
{
    if (character_physics_support())
    {
        character_physics_support()->PHGetLinearVell(velocity);
    }
    else
        inherited::PHGetLinearVell(velocity);
}

void CEntityAlive::set_lock_corpse(bool b_l_corpse)
{
    if (b_eating && !b_l_corpse)
        m_used_time = Device.dwTimeGlobal;

    b_eating = b_l_corpse;
}

bool CEntityAlive::is_locked_corpse()
{
    if (!b_eating)
    {
        if (m_used_time + m_use_timeout > Device.dwTimeGlobal)
            return true;
    }
    return b_eating;
}

CIKLimbsController* CEntityAlive::character_ik_controller()
{
    if (character_physics_support())
    {
        return character_physics_support()->ik_controller();
    }
    else
    {
        return NULL;
    }
}
CPHSoundPlayer* CEntityAlive::ph_sound_player()
{
    if (character_physics_support())
    {
        return character_physics_support()->ph_sound_player();
    }
    else
    {
        return NULL;
    }
}

ICollisionHitCallback* CEntityAlive::get_collision_hit_callback()
{
    CCharacterPhysicsSupport* pCPS = character_physics_support();
    if (pCPS)
        return pCPS->get_collision_hit_callback();

    return nullptr;
}

void CEntityAlive::set_collision_hit_callback(ICollisionHitCallback* cc)
{
    CCharacterPhysicsSupport* pCPS = character_physics_support();
    if (pCPS)
        pCPS->set_collision_hit_callback(cc);
}

void CEntityAlive::net_Relcase(CObject* object)
{
    inherited::net_Relcase(object);
    conditions().remove_links(object);
}

void CEntityAlive::create_anim_mov_ctrl(CBlend* b)
{
    inherited::create_anim_mov_ctrl(b);
    CCharacterPhysicsSupport* cs = character_physics_support();
    if (cs)
        cs->on_create_anim_mov_ctrl();
}
void CEntityAlive::destroy_anim_mov_ctrl()
{
    inherited::destroy_anim_mov_ctrl();
    CCharacterPhysicsSupport* cs = character_physics_support();
    if (cs)
        cs->on_destroy_anim_mov_ctrl();
}
