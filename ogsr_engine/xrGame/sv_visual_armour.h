#pragma once

// =====================================================================
//  What a creature's MODEL says about its protection.
//
//  ⭐⭐⭐The finding this file exists for (05-08-2026): in this content the
//  armour of an NPC is a property of its .ogf, not of its section. Every
//  creature section in system.ltx -- `stalker` included -- declares NO
//  bones_protection_sect at all; the line lives in the model's userdata
//  chunk, which is one #include:
//
//      meshes\actors\dolg\stalker_do_antigas.ogf, chunk 17:
//          #include "models\capture\kombez_antigas_damage_low.ltx"
//      ...which declares:
//          [bone_protection] bones_protection_sect = kombez_antigas_damage
//          [immunities]      immunities_sect       = <outfit immunities>
//
//  That is exactly the lookup CAI_Stalker::net_Spawn does through
//  IKinematics::LL_UserData (ai_stalker.cpp). SVDamage::BoneArmourFor asked
//  pSettings for the same key on the creature's SECTION instead, where it
//  has never existed, so every measurement the server ever made reported
//  barm=0.000 -- and that read as "this creature has no armour" rather than
//  as "the server is asking the wrong file".
//
//  ⭐The consequence worth keeping in mind: choosing a creature's MODEL is
//  choosing its armour. Measured over the 257 character models in this
//  install, 253 carry userdata naming one of 25 protection sets, from
//  kurtka_mask_damage_no (torso 0.20) to heavy armour (0.70). An AK round
//  lands at 1.0625 on a torso here, so that spread is the difference
//  between dying to one bullet and soaking three.
//
//  ⚠A headless host cannot get this through the engine's own path: its
//  visuals come up on the null kinematics stub, whose LL_UserData() returns
//  nullptr, so CAI_Stalker never builds m_boneHitProtection for anybody.
//  This reads the chunk straight out of the file, the same way sv_bone_names
//  reads chunk 13 for bone ids -- no renderer, no vertices, no animation,
//  and by construction the same answer the client's skeleton would give.
// =====================================================================

#include "sv_damage_model.h"

namespace SVVisualArmour
{
// What a model's userdata declares. `found` is false when the visual has no
// userdata chunk, when it names no protection, or when the file could not be
// read -- all three mean "no armour from the model", and the distinction is
// reported by Audit rather than folded into the damage path.
struct Model
{
    bool found{false};
    shared_str protection; // bones_protection_sect
    shared_str immunities; // immunities_sect, empty when the model overrides none
};

// Cached per visual. `visual` is CSE_Visual::visual_name / cNameVisual, with
// or without the .ogf suffix.
const Model& For(const shared_str& visual);

// Fill the cache for `visual` from the hit creature's loaded skeleton
// (LL_UserData), when this process has one -- instead of For() reading the
// whole .ogf for its userdata on the first hit. 📏07-10-2026 Switch v219:
// sv.hit.npc_health 21 ms of the second thread. No-op when cached already.
void Prime(const shared_str& visual, u16 owner);

// The per-bone protection this model gives, ready to subtract. Falls back to
// the creature's section when the model declares none, so a mod that does put
// the line where the old code looked for it keeps working.
SVDamage::BoneArmour BoneArmourFor(const shared_str& visual, const shared_str& section, const char* bone_name);

// Immunities the model overrides, layered over what the section already gave.
//
// ⚠Order matters and it is the engine's: CEntityAlive::Load reads the
// section's immunities_sect, then CAI_Stalker::net_Spawn re-reads from the
// model's userdata over the top. A model that names none leaves `out` alone.
// Returns true when the model had something to say.
bool ApplyImmunities(const shared_str& visual, float out[]);

// Normalise an operator-typed model name and confirm the file is there.
//
// ⚠⚠Not politeness. A spawn packet naming a model that does not exist reaches
// EVERY client in the world and each of them fails to load it, so one typo is a
// fault delivered to everybody. The server has no renderer and cannot check
// much about a model, but it can check that it exists -- and that is the
// mistake an operator actually makes.
//
// Accepts "actors\dolg\stalker_do_antigas", the same with .ogf, forward
// slashes, and the "meshes\..." prefix the .db listings print.
bool Resolve(const char* in, string_path& out);

// Level change. Visuals do not change under a running process, but nothing
// should outlive the world it was read for.
void Reset();

// sv_npc_armour [visual|filter] -- what protection the server believes a model
// carries, and the numbers behind it. With no argument, every visual asked for
// so far.
void Audit(const char* filter);

// sv_npc_models [filter] -- the catalogue: every character model the install
// has, what protection set it carries and how much that is worth on the torso
// and the head. ⭐This is what makes picking a model an informed decision
// rather than a guess at a filename.
void Catalogue(const char* filter);
} // namespace SVVisualArmour
