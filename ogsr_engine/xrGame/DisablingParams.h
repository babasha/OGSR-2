#pragma once

struct SOneDDOParams
{
    void Mul(float v);
    float velocity;
    float acceleration;
};

struct SAllDDOParams
{
    void Reset();
    void Load(CInifile* ini);
    SOneDDOParams translational;
    SOneDDOParams rotational;
    u16 L2frames;
};

struct SAllDDWParams
{
    SAllDDOParams objects_params;
    float reanable_factor;
};

extern SAllDDWParams worldDisablingParams;
extern float g_ph_disable_k; // ph_disable_k: multiplier of the per-step sleep thresholds (DisablingParams.cpp)
