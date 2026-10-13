#pragma once

// Which bone a hit landed on, for a process that has no skeletons.
//
// ⭐The gap this closes: a GE_HIT carries a bone ID, and every bone-dependent
// number in the content is keyed by bone NAME. Turning one into the other
// normally needs the victim's loaded skeleton, which a headless server does
// not have -- its visuals come up through the null kinematics stub, where
// LL_BoneID answers BI_NONE to everything and LL_BoneCount is 0. So the whole
// bone layer of the damage model was dead on the server, and sv_damage_model
// said so in its header: "hence a single assumed factor".
//
// ⭐But a bone ID is just an index into the order the names appear in the .ogf,
// and that order is a plain list in chunk OGF_S_BONE_NAMES. Reading it needs a
// file reader and nothing else -- no renderer, no vertex data, no animation.
// The server can therefore know exactly where it was hit, from the same file
// the client builds its skeleton from, which is what makes the two agree.
namespace SVBones
{
// Bone name for an id, "" when the visual has no bone list or the id is out of
// range. `visual` is CSE_Visual::visual_name / cNameVisual, with or without the
// .ogf suffix.
// `owner`: the object hit, when known -- its loaded skeleton answers without a file read.
const char* Name(const shared_str& visual, u16 bone_id, u16 owner = 0xffff);

// How many bones the visual declares; 0 when it is not a skeleton (or the file
// could not be read, which is logged once per visual).
u16 Count(const shared_str& visual);

// Level change: visuals do not change under a running process, but nothing
// should outlive the world it was read for.
void Reset();

// sv_bones [visual] -- what the server thinks the skeleton is. The first
// question to ask when a bone factor looks wrong is whether the id resolved at
// all, and this is where that is visible.
void Audit(const char* visual);
} // namespace SVBones
