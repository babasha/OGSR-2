#include "StdAfx.h"
#include "DisablingParams.h"

SAllDDWParams worldDisablingParams = {
    // object
    {
        {0.001f, 0.1f}, // translational		vel	,	accel
        {0.005f, 0.05f}, // rotational		vel	,	accel
        64 // level2 frames 2^
    },
    1.5f // reanable factor
};

// ph_disable_k (11-10-2026, Switch). The thresholds above are what a body moves (and how much its velocity changes)
// in ONE physics step, and they were tuned for the stock 100 Hz. The Switch steps at 50 Hz (Physics.cpp, v64): the same
// real velocity is twice the per-step displacement, and a body lying on the ground jitters ~4x more per step (g*dt^2),
// so props that should sleep never did -- [ph avg] in the Cordon village: ~37 islands awake every step, ~20
// CPhysicObject + ~14 CDestroyablePhysicsObject, ~1.5 of the 2.4 ms a step. 2 = the stock real-velocity thresholds.
float g_ph_disable_k = 1.f;

void SOneDDOParams::Mul(float v)
{
    velocity *= v;
    acceleration *= v;
}

void SAllDDOParams::Reset() { *this = worldDisablingParams.objects_params; }

void SAllDDOParams::Load(CInifile* ini)
{
    Reset();
    if (!ini)
        return;
    if (!ini->section_exist("disable"))
        return;
    if (ini->line_exist("disable", "linear_factor"))
        translational.Mul(ini->r_float("disable", "linear_factor"));
    if (ini->line_exist("disable", "angular_factor"))
        rotational.Mul(ini->r_float("disable", "angular_factor"));
    if (ini->line_exist("disable", "change_count"))
    {
        int ch_cnt = ini->r_s8("disable", "change_count");
        if (ch_cnt < 0)
            L2frames = L2frames >> u16(-ch_cnt);
        else
            L2frames = L2frames << u16(ch_cnt);
        VERIFY(ch_cnt < 4 && L2frames != 0);
    }
}