#pragma once

// See stalker_animation_trace.cpp for what this is for. One line per frame per
// traced stalker, in a format that is IDENTICAL in single player and on a
// network client -- so the two can be diffed against each other.

class CAI_Stalker;

extern int g_cl_anim_trace;
extern int g_cl_anim_trace_id;

namespace StalkerAnimTrace
{
// Called from CStalkerAnimationManager::update -- the one place that runs for
// whoever is driving the animator, single player or puppet.
void Frame(CAI_Stalker* S);
} // namespace StalkerAnimTrace
