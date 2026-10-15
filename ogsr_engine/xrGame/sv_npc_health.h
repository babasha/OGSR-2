#pragma once

class NET_Packet;
class CSE_Abstract;

// The server's own health for creatures nobody else owns.
//
// ⚠⭐WHY THIS EXISTS AT ALL, measured 27-07: sixteen point-blank AK hits were
// relayed to every client and the stalker did not lose a single point. The
// engine's damage path runs on the victim's simulating side and is built on
// its SKELETON -- CAI_Stalker::Hit reads m_boneHitProtection, which
// SBoneProtections::reload fills by resolving bone NAMES through
// IKinematics::LL_BoneID. A headless host answers BI_NONE to every one of them,
// so nothing downstream of that has anything to work with. The hit was
// recorded perfectly and cost nobody anything.
//
// ⭐So the server stops asking anyone else. For a creature no client controls
// there is no competing authority and no reason to bound the damage instead of
// computing it: this is a LEDGER OF RECORD, not the lower bound sv_health.h
// keeps for players. The two must not be confused --
//   * players: the client simulates, so the server holds a floor and can only
//     ever re-anchor DOWNWARD (see sv_health.h);
//   * creatures: the server simulates, so the server's number IS the health.
//
// What it can now read that it previously could not: WHICH BONE. SVBones names
// the bone straight out of the .ogf, so the per-bone factor and the creature's
// own bones_protection table both apply for real, from the same content the
// client would have used.
namespace SVNpcHealth
{
// 0 = off, 1 = compute and log only, 2 = authoritative (subtract, and kill at
// zero). Console: sv_npc_health.
extern int g_sv_npc_health;

// A relayed GE_HIT. Called from game_sv_base AFTER the drop gate, because a hit
// the server refused to deliver must not cost the victim anything -- the same
// rule the player ledger follows. Cursor must be at the victim id; it is
// restored.
void OnHit(NET_Packet& P);

// ⭐The creature's own game object just paid for a hit, through the engine's
// damage path and the mod's scripts. Called from CEntityAlive::Hit on the host.
//
// This is the other half of OnHit, and the split is the whole point: for a
// creature the engine can charge, THIS is the record -- what was really spent,
// including everything the ledger has no way to know (measured 28-07: the mod
// multiplies weapon hits by 1.7 in a Lua callback). OnHit steps aside for those
// and stays authoritative only where nothing else can apply the damage.
void NoteApplied(u16 victim, u16 attacker, u16 bone, u16 hit_type, float wire_power, float applied_power, float hp_before, float hp_after);

// ⭐Health that left without a hit: bleeding from an open wound, and anything
// else CEntityCondition spends between blows. Called from the condition update
// on the host, and only for a creature already on the books -- an unhit
// creature has nothing here to keep in step.
//
// ⚠⚠WHY IT MATTERS BEYOND TIDINESS, measured 30-07-2026: a fracture was shot
// down to 0.0165 and then BLED OUT. NoteApplied had recorded the last hit
// faithfully, the ledger stood at 0.0165, and the engine's own object went to
// -0.01 two drain ticks later. The death then tripped the second-authority
// watchdog -- "the server's record had it at hp=0.0165" -- on a death that was
// entirely ordinary. A record that only hears about bullets will always
// disagree with a world where creatures also bleed.
//
// ⭐The false alarm was the cheap half of the cost. The expensive half is that
// it had been read as evidence of a real defect for two days.
void NoteDrain(u16 id, float hp_after);

// True when the ENGINE spends this creature's health and the ledger only
// writes down what it spent.
//
// ⭐⭐The distinction the second-authority watchdog turns on, because the two
// cases need OPPOSITE questions asked of them:
//   * ledger is the authority -> "is it still holding health for something the
//     world just killed?" A yes means a second authority grew back.
//   * ledger is a record -> that question is meaningless (the engine is SUPPOSED
//     to be the one that killed it). The question that still has teeth is "did
//     the record keep up with what the engine spent?", answered against
//     LiveHealth below.
bool EngineOwns(u16 id);

// What the object that actually spends the health is holding right now. False
// if this process has no live object for that id.
//
// ⚠Not the same as the server entity's fHealth, which is published a frame
// later -- measured 30-07-2026 at a death: entity 0.1612, object -0.0100.
bool LiveHealth(u16 id, float& hp);

// Creature left the world: its entry goes with it, and ids get reused.
void Forget(u16 id);
void Reset();

// sv_npc_health_dump -- who is on the books and what is left of them.
void Dump();

// A creature the SERVER simulates, as opposed to a body some client drives.
// ⚠The test is the reverse link (a client points at its entity), not
// `owner == nullptr`: the host's in-process client owns the entire level, so
// every creature on the map has an owner.
bool IsServerCreature(u16 id);

// Copy a creature's real health from the object that owns it into the server
// entity everybody else reads.
//
// ⚠Needed because a death is announced INSIDE the hit that caused it: the
// engine sends GE_DIE from KillEntity, which runs deep inside CEntity::Hit, and
// the in-process client's export (CLevel::ClientSend) does not run again until
// the next frame. Measured 28-07: three kills recorded with "vhp":1.000 on
// creatures that had just been shot to -0.01. The record was written a frame
// before the truth caught up with it.
void PublishHealth(CSE_Abstract* e);

// What the ledger believes about one creature, for code that has to report a
// disagreement with it. False if the creature was never hit. See dbg_death.
bool Ledger(u16 id, float& hp, bool& dead);

// sv_npc_damage_test <id> <power> [bone] -- run one hit through the whole
// calculation without a shooter, and print every factor that went into it.
// The arithmetic is the part that is easy to get quietly wrong.
void Explain(u16 id, float power, u16 bone);
} // namespace SVNpcHealth
