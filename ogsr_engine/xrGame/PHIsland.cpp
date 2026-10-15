#include "StdAfx.h"
#include "PHIsland.h"
#include "Physics.h"
// ph_exact_joints (12-10-2026, Switch): the stock limit was the constant max_joint_allowed_for_exeact_integration (30).
// An island of a shell with fixed bones (a fan, a lamp, a prop nailed to the level) is stepped by the exact solver --
// cubic in the constraint rows, contacts included: dInternalStepIsland_x2 was 20 % of the physics of the 12-bandit
// fight, as much as the iterative solver for all the rest. 0 = everything through the iterative one.
int g_ph_exact_joints = 30; // = max_joint_allowed_for_exeact_integration (Physics.cpp)

void CPHIsland::Step(dReal step)
{
    if (!m_flags.is_active())
        return;
    // dWorldStepFast1	(DWorld(),	fixed_step,	phIterations/*+Random.randI(0,phIterationCycle)*/);
    if (m_flags.is_exact_integration_prefeared() && nj < g_ph_exact_joints)
        dWorldStep(DWorld(), fixed_step);
    else
        dWorldQuickStep(DWorld(), fixed_step);
    // dWorldStep(DWorld(),fixed_step);
}

void CPHIsland::Enable()
{
    if (!m_flags.is_active())
        return;

    for (dxBody* body = DWorld()->firstbody; body; body = (dxBody*)body->next)
        body->flags &= ~dxBodyDisabled;
}
void CPHIsland::Repair()
{
    if (!m_flags.is_active())
        return;
    dBodyID body;
    for (body = firstbody; body; body = (dxBody*)body->next)
    {
        if (!dV_valid(dBodyGetAngularVel(body)))
            dBodySetAngularVel(body, 0.f, 0.f, 0.f);
        if (!dV_valid(dBodyGetLinearVel(body)))
            dBodySetLinearVel(body, 0.f, 0.f, 0.f);
        if (!dV_valid(dBodyGetPosition(body)))
            dBodySetPosition(body, 0.f, 0.f, 0.f);
        if (!dQ_valid(dBodyGetQuaternion(body)))
        {
            dQuaternion q = {1.f, 0.f, 0.f, 0.f}; // dQSetIdentity(q);
            dBodySetQuaternion(body, q);
        }
    }
}