void CPHSimpleCharacter::UpdateStaticDamage(dContact* c, SGameMtl* tri_material, bool bo1)
{
    const dReal* v = dBodyGetLinearVel(m_body);
    dReal norm_prg = dFabs(dDOT(v, c->geom.normal));
    dReal smag = dDOT(v, v);
    dReal plane_pgr = _sqrt(smag - norm_prg * norm_prg);
    dReal mag = 0.f;
    if (tri_material->Flags.test(SGameMtl::flPassable))
    {
        mag = _sqrt(smag) * tri_material->fBounceDamageFactor;
    }
    else
    {
        float vel_prg;
        vel_prg = _max(plane_pgr * tri_material->fPHFriction, norm_prg);
        mag = (vel_prg)*tri_material->fBounceDamageFactor;
    }
    if (mag > m_collision_damage_info.m_contact_velocity)
    {
        m_collision_damage_info.m_contact_velocity = mag;
        m_collision_damage_info.m_dmc_signum = bo1 ? 1.f : -1.f;
        m_collision_damage_info.m_dmc_type = SCollisionDamageInfo::ctStatic;
        m_collision_damage_info.m_damege_contact = *c;
        // m_collision_damage_info.m_object			=	0;
        m_collision_damage_info.m_obj_id = u16(-1);
    }
}

void CPHSimpleCharacter::UpdateDynamicDamage(dContact* c, u16 obj_material_idx, dBodyID b, bool bo1)
{
    // ⭐⭐⭐WALKING INTO A PERSON IS NOT AN ACCIDENT.
    //
    // USER-REPORTED 31-07-2026: "получаю урон от НПС от столкновения, это
    // странно". Measured before it was believed -- the server's own hit ledger
    // named every hit of the session:
    //
    //   strike power 0.0797  bone 11  from 0 ('single_player') with 'actor'
    //   strike power 0.6519  bone 7   from 0 ('single_player') with 'actor'
    //
    // Nobody shot him. This function turned the contact between two character
    // capsules into a landing, and the landing killed him. The formula below is
    // written for falling onto WORLD GEOMETRY or being run over by a heavy
    // dynamic body: it takes the closing energy along the contact normal and
    // charges the difference to whoever absorbed it. A body it cannot walk
    // through is exactly what another character is, so every shove, every
    // doorway squeeze and every step onto a stalker's shoulder is billed as a
    // crash.
    //
    // ⭐Not a threshold change. Raising fMinCrashSpeed would also stop real
    // falls from hurting, and the two are different events that happen to share
    // one number. The distinction the engine already draws is WHAT was hit --
    // CPHObject::tpCharacter -- and that is what is asked here.
    //
    // ⚠Monsters keep their teeth: a pseudogiant's charge, a burer's throw and
    // every melee attack are explicit HitEntity calls with their own power, not
    // collision damage. Cars and thrown physics objects are not characters and
    // are untouched. What this removes is only the accidental kind.
    // ⚠A LOCAL scale, never the member. m_collision_damage_factor persists for
    // the life of the character, and multiplying it here would compound once
    // per contact per step -- the knob would silently drive every character's
    // factor to zero within a second of touching anybody.
    float self_damage_factor = m_collision_damage_factor;
    if (ph_character_collision_damage < 1.f)
    {
        const dxGeomUserData* ud_other = bo1 ? retrieveGeomUserData(c->geom.g2) : retrieveGeomUserData(c->geom.g1);
        if (ud_other && ud_other->ph_object && ud_other->ph_object->CastType() == CPHObject::tpCharacter)
        {
            if (ph_character_collision_damage <= 0.f)
            {
                // ⭐Say it when it MATTERED. "The fix works" and "the fix never
                // fires because the diagnosis was wrong" look identical from
                // outside, and only the second is worth knowing about.
                //
                // ⚠Gated on a real thump, not on the contact: two stalkers
                // standing shoulder to shoulder generate these every step, and
                // a line per contact would bury the log without saying
                // anything. 3 m/s of closing speed is a collision somebody
                // felt.
                const dReal* v_self = dBodyGetLinearVel(m_body);
                const dReal* v_other = dBodyGetLinearVel(b);
                const dReal closing = dFabs(dDOT(v_self, c->geom.normal) - dDOT(v_other, c->geom.normal));
                if (closing > 3.f)
                {
                    static u32 s_said = 0;
                    ++s_said;
                    if (s_said <= 12 || 0 == (s_said % 200))
                        Msg("~ [phys] '%s' hit another character at %.2f m/s closing -- collision damage suppressed (%u so far)",
                            PhysicsRefObject() ? PhysicsRefObject()->cName().c_str() : "?", float(closing), s_said);
                }
                return;
            }
            // Scaled rather than dropped, so the knob has a middle.
            self_damage_factor *= ph_character_collision_damage;
        }
    }

    // if(ph_world ->IsFreezed())
    // return;
    const dReal* vel = dBodyGetLinearVel(m_body);
    dReal c_vel;
    dMass m;
    dBodyGetMass(b, &m);

    const dReal* obj_vel = dBodyGetLinearVel(b);
    const dReal* norm = c->geom.normal;
    dReal norm_vel = dDOT(vel, norm);
    dReal norm_obj_vel = dDOT(obj_vel, norm);

    if ((bo1 && norm_vel > norm_obj_vel) || (!bo1 && norm_obj_vel > norm_vel))
        return;

    dVector3 Pc = {vel[0] * m_mass + obj_vel[0] * m.mass, vel[1] * m_mass + obj_vel[1] * m.mass, vel[2] * m_mass + obj_vel[2] * m.mass};
    // dVectorMul(Vc,1.f/(m_mass+m.mass));
    // dVector3 vc_obj={obj_vel[0]-Vc[0],obj_vel[1]-Vc[1],obj_vel[2]-Vc[2]};
    // dVector3 vc_self={vel[0]-Vc[0],vel[1]-Vc[1],vel[2]-Vc[2]};
    // dReal vc_obj_norm=dDOT(vc_obj,norm);
    // dReal vc_self_norm=dDOT(vc_self,norm);

    dReal Kself = norm_vel * norm_vel * m_mass / 2.f;
    dReal Kobj = norm_obj_vel * norm_obj_vel * m.mass / 2.f;

    dReal Pcnorm = dDOT(Pc, norm);
    dReal KK = Pcnorm * Pcnorm / (m_mass + m.mass) / 2.f;
    dReal accepted_energy = Kself * self_damage_factor + Kobj * object_damage_factor - KK;
    // DeltaK=m1*m2*(v1-v2)^2/(2*(m1+m2))
    if (accepted_energy > 0.f)
    {
        SGameMtl* obj_material = GMLib.GetMaterialByIdx(obj_material_idx);
        c_vel = dSqrt(accepted_energy / m_mass * 2.f) * obj_material->fBounceDamageFactor;
    }
    else
        c_vel = 0.f;
#ifdef DEBUG
    if (ph_dbg_draw_mask.test(phDbgDispObjCollisionDammage) && c_vel > dbg_vel_collid_damage_to_display)
    {
        float dbg_my_norm_vell = norm_vel;
        float dbg_obj_norm_vell = norm_obj_vel;
        float dbg_my_kinetic_e = Kself;
        float dbg_obj_kinetic_e = Kobj;
        float dbg_my_effective_e = Kself * self_damage_factor;
        float dbg_obj_effective_e = Kobj * object_damage_factor;
        float dbg_free_energy = KK;
        Msg("-----------------------------------------------------------------------------------------");
        Msg("cd %s -effective vell %f", *PhysicsRefObject()->cName(), c_vel);
        Msg("cd %s -my_norm_vell %f", *PhysicsRefObject()->cName(), dbg_my_norm_vell);
        Msg("cd %s -obj_norm_vell %f", *PhysicsRefObject()->cName(), dbg_obj_norm_vell);
        Msg("cd %s -my_kinetic_e %f", *PhysicsRefObject()->cName(), dbg_my_kinetic_e);
        Msg("cd %s -obj_kinetic_e %f", *PhysicsRefObject()->cName(), dbg_obj_kinetic_e);
        Msg("cd %s -my_effective_e %f", *PhysicsRefObject()->cName(), dbg_my_effective_e);
        Msg("cd %s -obj_effective_e %f", *PhysicsRefObject()->cName(), dbg_obj_effective_e);
        Msg("cd %s -effective_acceted_e %f", *PhysicsRefObject()->cName(), accepted_energy);
        Msg("cd %s -real_acceted_e %f", *PhysicsRefObject()->cName(), Kself + Kobj - KK);
        Msg("cd %s -free_energy %f", *PhysicsRefObject()->cName(), dbg_free_energy);
        Msg("-----------------------------------------------------------------------------------------");
        /*
        static float dbg_my_norm_vell=0.f;
        static float dbg_obj_norm_vell=0.f;
        static float dbg_my_kinetic_e=0.f;
        static float dbg_obj_kinetic_e=0.f;
        static float dbg_my_effective_e=0.f;
        static float dbg_obj_effective_e=0.f;
        static float dbg_free_energy=0.f;
        if()
            dbg_my_norm_vell=norm_vel;
            dbg_obj_norm_vell=norm_obj_vel;
            dbg_my_kinetic_e=Kself;
            dbg_obj_kinetic_e=Kobj;
            dbg_my_effective_e=Kself*m_collision_damage_factor;
            dbg_obj_effective_e=Kobj*object_damage_factor;
            dbg_free_energy=KK;
        DBG_OutText("-----dbg obj collision damage-------");
        DBG_OutText("my_norm_vell %f",dbg_my_norm_vell);
        DBG_OutText("obj_norm_vell %f",dbg_obj_norm_vell);
        DBG_OutText("my_kinetic_e %f",dbg_my_kinetic_e);
        DBG_OutText("obj_kinetic_e %f", dbg_obj_kinetic_e);
        DBG_OutText("my_effective_e %f",dbg_my_effective_e);
        DBG_OutText("obj_effective_e %f",dbg_obj_effective_e);
        DBG_OutText("free_energy %f",dbg_free_energy);
        DBG_OutText("-----------------------------------");
        */
    }
#endif
    if (c_vel > m_collision_damage_info.m_contact_velocity)
    {
        CPhysicsShellHolder* obj = bo1 ? retrieveRefObject(c->geom.g2) : retrieveRefObject(c->geom.g1);
        VERIFY(obj);
        if (!obj->getDestroy())
        {
            m_collision_damage_info.m_contact_velocity = c_vel;
            m_collision_damage_info.m_dmc_signum = bo1 ? 1.f : -1.f;
            m_collision_damage_info.m_dmc_type = SCollisionDamageInfo::ctObject;
            m_collision_damage_info.m_damege_contact = *c;
            m_collision_damage_info.m_hit_callback = obj->get_collision_hit_callback();
            m_collision_damage_info.m_obj_id = obj->ID();
        }
        else
        {
            m_collision_damage_info.Reinit();
        }
    }
}

IC void CPHSimpleCharacter::foot_material_update(u16 contact_material_idx, u16 foot_material_idx)
{
    if (*p_lastMaterialIDX != u16(-1) && GMLib.GetMaterialByIdx(*p_lastMaterialIDX)->Flags.test(SGameMtl::flPassable) && !b_foot_mtl_check)
        return;
    b_foot_mtl_check = false;
    if (GMLib.GetMaterialByIdx(contact_material_idx)->Flags.test(SGameMtl::flPassable))
        *p_lastMaterialIDX = contact_material_idx;
    else
        *p_lastMaterialIDX = foot_material_idx;
}