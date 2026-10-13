#pragma once

// Smoothness of a creature ON SCREEN, measured on its final bone transforms.
// See the .cpp for why bones, and why the answer is a histogram of intervals.
//
//    cl_bone_audit <id>   record   |   cl_bone_audit 0   stop and report
namespace ClBoneAudit
{
// Per frame, from CLevel::OnFrame -- after the puppet interpolator and after
// the animator, so it samples what will actually be drawn.
void Frame_();

void Start(u16 id);
void Stop();

// cl_stalkers: id, distance and speed of every alive stalker nearby, so a
// harness can pick a subject by what it is DOING instead of by a guessed id.
void List();
} // namespace ClBoneAudit
