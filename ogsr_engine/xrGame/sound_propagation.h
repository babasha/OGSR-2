#pragma once

// ⭐snd_prop (08-10-2026): the sound's way round obstacles, from the AI map. See sound_propagation.cpp.
namespace SoundPropagation
{
void Frame(); // hands the level's paths to the sound once the AI map is there
void Reset(); // before the AI map goes (~CLevel)
} // namespace SoundPropagation
