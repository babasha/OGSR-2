#pragma once

// Being shot at, from the player's side: bullets passing close or striking
// next to him flinch the camera, keep a tremor going while the fire lasts and
// widen his own spread. Without it the AI's suppressing fire was only a noise:
// behind cover he was perfectly safe and felt it. The bullet manager reports
// near misses (CBulletManager::CalcBullet), the local actor applies the rest.
class CActor;

namespace ActorSuppression
{
// main thread: closeness of each bullet summed, 1 = through the eyes, 0 = at the edge
void near_miss(float closeness_sum, u32 count);
// the local actor's UpdateCL: decay, and the camera effector while it lasts
void update(CActor& actor);
// the player's spread multiplier, CActor::GetWeaponAccuracy
float dispersion_factor();
float level();
} // namespace ActorSuppression
