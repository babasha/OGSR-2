#pragma once

// Aim assist for a pad (Switch, Android): the stick is a poor pointer -- the Joy-Con
// one most of all -- so near an enemy the game helps the way console shooters do:
//   1. friction   -- the look slows down over the target, the crosshair does not slide off it;
//   2. tracking   -- the camera follows the target's angular motion (its run, the player's strafe);
//   3. pull       -- while the stick moves towards the target, a gentle turn onto it;
//   4. snap       -- aiming down the sights (ZL) near a target turns onto it in a fraction of a second;
//   5. bullet     -- a shot that misses the aim point by a little is bent onto it.
// Only enemies the player can see, only with a firearm, and only while the look comes from a pad
// (g_pad_look, written by the pad backend); `aim_assist 2` turns it on for the mouse too (a PC test).
class CActor;

namespace AimAssist
{
// the local actor's UpdateCL: pick the target, follow it, the snap on aiming down the sights
void update(CActor& actor);
// CActor::IR_OnMouseMove: the look multiplier near the target (1 = no slowdown)
float look_factor();
// CActor::g_fireParams: bend the shot onto the target's aim point when the miss is small
void bend_shot(const Fvector& fire_pos, Fvector& fire_dir);
// `aa_test snap [off=6] | track [time=4] | stop`: the unattended check -- the crosshair is put off the nearest visible
// enemy and what the assist does to it is measured and written to the log (run it with aim_assist 0 for the baseline)
void test_command(LPCSTR args);
} // namespace AimAssist
