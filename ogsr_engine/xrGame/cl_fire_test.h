#pragma once

// Pulls the player's trigger without a player, so the shot-to-record latency in
// shot_latency.h can be measured on the path a person's shot actually takes.
//
// ⭐Everything here goes through the real chain: the actor's own camera is
// pointed at a creature and the trigger is pressed through
// CActor::IR_OnKeyboardPress(kWPN_FIRE). Nothing synthesises a bullet or a
// GE_HIT. A test that injected the packet would confirm the packet handler and
// tell us nothing about aiming, dispersion, the weapon state machine, or
// whether a server-driven puppet can even be shot at.
//
// ⭐The aim is a closed loop, not trigonometry: the camera's yaw/pitch-to-world
// mapping is MEASURED by nudging each axis and watching which way the direction
// moved. Guessing the sign convention (and there are two plausible ones) would
// have cost a build and a run to discover, and the loop also absorbs the
// camera effectors that a one-shot calculation cannot.
namespace CLFire
{
// Fire `shots` rounds at the nearest creature within `max_range` metres,
// leaving `gap_ms` between trigger pulls.
void Start(int shots, u32 gap_ms, float max_range);
// 12-10-2026: the same trigger, aimed at a fixed point of the world instead of a creature (shots into the water)
void StartAtPoint(int shots, u32 gap_ms, const Fvector& point);
void Stop();
void Status();

// Once per client frame, from CLevel::OnFrame.
void Frame();
} // namespace CLFire
