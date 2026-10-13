#pragma once

extern float object_damage_factor;
extern float collide_volume_max;
extern float collide_volume_min;

// ⭐How much of the collision damage from ANOTHER CHARACTER a character
// accepts. 0 = none, 1 = the stock behaviour of treating a person like a wall
// you fell onto. Default 0: see the long comment in
// CPHSimpleCharacter::UpdateDynamicDamage for what the stock number did to a
// player who walked into a stalker.
//
// ⚠A knob rather than a deletion because "should landing on somebody from a
// roof hurt" is a design question, not a bug, and the answer belongs to whoever
// is balancing the game rather than to whoever found the defect.
extern float ph_character_collision_damage;

// ⭐The speed limit on ODE's penetration recovery. ODE ships dInfinity and this
// tree never set it, which let an overlap become a launch. Kept below the
// actor's fMinCrashSpeed so the solver's own error correction can never reach
// the damage model. 0 restores ODE's unbounded default for an A/B.
// See CPHWorld::Create for the measurement that produced this.
extern float ph_max_correcting_vel;

struct EffectPars
{
    const static float vel_cret_sound;
    const static float vel_cret_particles;
    const static float vel_cret_wallmark;
};

struct CharacterEffectPars
{
    const static float vel_cret_sound;
    const static float vel_cret_particles;
    const static float vel_cret_wallmark;
};

void LoadPhysicsGameParams();