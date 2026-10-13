#pragma once

#include "../COMMON_AI/alife_space.h"

// =====================================================================
//  What content says about damage, as the server can read it.
//
//  The server never applies damage -- the owning client does -- but it has
//  to bound it (see sv_health.h), and every factor in that bound comes out
//  of ini. This is the reading half, kept apart from the ledger that uses
//  it so that "what does the content say" and "what do we do about it"
//  cannot drift into each other.
//
//  Two things the server CANNOT read, and both matter:
//    * which bone a hit landed on -- the packet carries a bone id, and
//      turning that into a name needs the victim's skeleton, which a
//      headless server does not load. Hence a single assumed factor.
//    * what a creature has EQUIPPED -- only what it owns. Armour is
//      therefore the best of what it carries, which errs towards less
//      damage, the safe direction for a lower bound.
//
//  The audit entry points exist because every one of these reads can fail
//  silently into a default, and a floor built from defaults is worthless
//  in a way nothing else reports. Measured twice already: a bone factor
//  that collapsed to zero, and an armour matcher that had to be shown
//  finding real outfits.
// =====================================================================

class CSE_Abstract;

// Bone damage factor assumed when a victim's damage section cannot be read
// (NPC bone factors live in visual user data, which a server with no visuals
// has no access to). Console: sv_health_bone_floor.
extern float g_sv_health_bone_floor;

namespace SVDamage
{
// The two factors the client applies that depend on neither equipment nor
// bone, so the server can reproduce them exactly rather than bound them.
struct Profile
{
    shared_str section;
    float immunity[ALife::eHitTypeMax];
    float health_part[ALife::eHitTypeMax];
    float bone_floor{1.f}; // smallest POSITIVE per-bone factor in the section
};

// Cached per section. Returned by value: the cache is a vector, so a
// reference into it dies the next time a section is added.
Profile ProfileFor(const shared_str& section);

// The per-bone factor the content declares for a named bone of a section --
// the `damage` line's table, the same one AuditDamage prints. Falls back to
// that table's `default`, then to 1.0.
//
// ⭐This is what Profile::bone_floor exists INSTEAD of when the bone is
// unknown. With SVBones able to name the bone a hit landed on, a server can
// now ask the real question rather than assume the least damaging answer.
float BoneFactor(const shared_str& section, const char* bone_name);

// Per-bone protection from the creature's own `bones_protection_sect`: the
// koeff/armour pair CAI_Stalker::Hit applies, plus the section's hit_fraction.
// Creatures carry no outfit, so this table is all the armour they have.
struct BoneArmour
{
    bool found{false};
    float koeff{1.f};
    float armour{0.f};
    float hit_fraction{0.1f};
};
BoneArmour BoneArmourFor(const shared_str& section, const char* bone_name);

// The same table read straight from a named protection section.
//
// ⭐Split out because in this content the section a creature protects itself
// with is named by its MODEL, not by its own ini -- see sv_visual_armour.h.
// Both callers must resolve the table identically or the server's arithmetic
// stops matching the client's, so there is exactly one reader for it.
BoneArmour ReadProtection(const char* prot_sect, const char* bone_name);

// Immunity multipliers from a named immunities section, written over `out`
// for every hit type the section declares. Returns false when the section
// does not exist, leaving `out` untouched.
bool ReadImmunities(const char* imm_sect, float out[]);

// =====================================================================
//  The most a single hit from a given source can possibly carry.
//
//  ⭐⭐⭐WHY: `power` in a GE_HIT packet is computed on the SHOOTER'S machine.
//  CShootingObject::FireBullet takes the weapon's hit_power, the round's
//  k_hit and a speed factor that only ever falls with distance, and the
//  client sends the result. Nothing on the way in ever questioned it, so a
//  patched client could ask for any number it liked and every authority
//  downstream -- the creature's own object, the player ledger, the
//  telemetry -- would faithfully apply what it was told.
//
//  The cap is exact and tight rather than a guess, because the speed factor
//  is at most 1: whatever ballistics happened on the client, an honest shot
//  cannot exceed hit_power x k_hit. Measured on an AK-74 firing 5.45 FMJ:
//  cap 0.6250, wire 0.6250 point blank and 0.6235 at range.
//
//  ⚠What this does NOT bound is WHERE the bullet landed -- victim and bone
//  are still the shooter's word. That needs its own work; do not mistake a
//  capped power for a validated hit.
//
//  ---------------------------------------------------------------------
//  ⭐⭐⭐PAST BULLETS (30-07-2026). For two days this only knew firearms, and
//  every other kind of damage -- knife, grenade, anomaly, fall -- was relayed
//  with whatever number arrived. The census (sv_hit_types) was built to wait
//  for the first non-bullet claim from a network client before capping
//  anything, on the rule that a ceiling nobody can justify from content will
//  refuse honest damage.
//
//  ⭐That wait was the wrong instrument for the question. The claim on the
//  wire says what a client DID ask for; what a ceiling needs is what content
//  ALLOWS, and that is readable right now, from the same ini lines the
//  client's own code reads to produce the hit:
//
//    firearm bullet   hit_power x round k_hit          CShootingObject::FireBullet
//    knife / melee    hit_power, hit_power_2           CWeaponKnife::LoadFireParams
//    blast            blast                            CExplosive::Explode
//    fragments        frag_hit                         CExplosive (via BulletManager)
//    anomaly          max_start_power                  CCustomZone::Power
//
//  Each producer also declares the TYPE it makes (hit_type, hit_type_2,
//  hit_type_blast, hit_type_frag), so the ceiling is per (section, type) and
//  not per section: a grenade's blast and its fragments are different hits
//  with different numbers, and capping one with the other's line would be
//  wrong in both directions.
//
//  ⚠What still has NO ceiling, deliberately, and passes untouched:
//    * collision and fall damage -- physics computes it, no content line
//      bounds it (PHCollisionDamageReceiver);
//    * monster melee -- the number lives in the attack ANIMATION table, not
//      in one line, and the host simulates monsters anyway, so a claim of it
//      from a client has never been seen;
//    * script hits -- a mod can send any type from Lua, legitimately.
//  Unknown means pass. A cap that guesses is worse than no cap: it refuses
//  real damage, and the symptom reads as a netcode bug.
// =====================================================================
struct ShotCap
{
    bool known{false}; // false = nothing to compare against, let the hit pass
    float power{0.f};
    shared_str weapon; // sections that went into it, so a report can name them
    shared_str ammo;
    bool exact_ammo{false}; // false = capped over every round the weapon takes

    // Which content line the ceiling came out of, so a refusal can be argued
    // with rather than believed.
    const char* how{"-"};

    // The source is something a shooter HOLDS, so "is it in the claimed
    // attacker's hands" is a question worth asking. False for a thrown
    // grenade, an anomaly or anything else that acts on its own in the world:
    // those have no parent to check, and demanding one would exempt them.
    bool carried{false};
};

// `source` is the server entity named by the hit packet's weapon id, `hit_type`
// the kind of damage it claims. Both are needed: one section can produce
// several kinds of hit, each with its own ceiling.
ShotCap PowerCapFor(CSE_Abstract* source, u16 hit_type = ALife::eHitTypeFireWound);

// A hit type by the name content spells it with, or -1 for anything else.
//
// ⚠ALife::g_tfString2HitType is the engine's version of this and it FATALs on
// an unrecognised string. That is defensible where content is trusted and fatal
// -- literally -- anywhere a report walks over sections nobody vetted, or a
// console command echoes back what an operator typed.
int HitTypeFromName(const char* name);

// The armour a victim could be wearing.
struct Armour
{
    bool found{false};
    float protection[ALife::eHitTypeMax];
    float hit_fraction{1.f}; // share of a bullet's power that always gets through

    Armour()
    {
        for (int i = 0; i < ALife::eHitTypeMax; ++i)
            protection[i] = 0.f;
    }
};

// Best protection among everything the owner carries.
void ResolveArmour(CSE_Abstract* owner, Armour& out);

// True for sections that protect -- outfits, helmets, mod-added pieces.
bool SectionIsArmour(const char* section);

// The artefacts a victim could have on their belt.
//
// CActor::HitArtefactsOnBelt runs BEFORE armour and scales the raw power, so
// this is a third multiplier on the floor and not a variation of the other
// two. It was missed when the ledger was written, and it is the one omission
// that can push the floor ABOVE real damage: a player carrying protective
// artefacts really does take less than the floor charges them.
struct Belt
{
    bool found{false};
    float k[ALife::eHitTypeMax]; // strongest reduction the victim could be getting
    u32 carried{0}; // protective artefacts the bound counted
    u32 excluded{0}; // ...and ones the client told us are not on the belt
    u32 slots{0}; // belt capacity the bound was capped to

    Belt()
    {
        for (int i = 0; i < ALife::eHitTypeMax; ++i)
            k[i] = 1.f;
    }
};

// Strongest belt the owner could be wearing out of what they carry.
void ResolveBelt(CSE_Abstract* owner, Belt& out);

// Where an item sits in its owner's inventory, as far as the server knows.
//
// ⚠The asymmetry here is the whole safety argument. Every path that puts an
// item ON the belt from the UI sends GEG_PLAYER_ITEM2BELT through the server --
// but CInventory::Take also places by the item's own m_eItemPlace when it is
// picked up or loaded, with no event at all. So a silent item may well be
// belted, and the server may NOT conclude "no event means no belt".
//
// Therefore: only a POSITIVE sighting of ITEM2RUCK/ITEM2SLOT excludes an item
// from the belt bound. Unknown stays counted, which is exactly the behaviour
// before any of this existed. Evidence can only ever tighten the bound.
enum class Place
{
    Belt, // client said ITEM2BELT
    NotBelt // client said ITEM2RUCK or ITEM2SLOT
};

// Record what a relayed inventory event revealed. Ignores anything that is not
// a protective artefact, so the registry stays the size of what it affects.
void NoteItemPlace(u16 item_id, Place place);

// The item changed hands, was dropped or destroyed -- whatever the server knew
// about where it sat no longer holds. CInventory::Take will place it again from
// its own state without telling us, so it has to go back to being unknown.
void ForgetItemPlace(u16 item_id);

// Level change or server shutdown.
void ResetPlaces();

// True for sections that absorb hits from the belt -- artefacts, and whatever
// a mod has given a hit_absorbation_sect to.
bool SectionIsArtefact(const char* section);

// Drop the per-section cache. Content does not change under a running
// process, but a level change can bring different sections into play and
// nothing should outlive the world it was read for.
void ResetCache();

// --- reports (console commands) ---------------------------------------
void AuditDamage(const char* section); // sv_damage_audit
void AuditArmour(); // sv_armour_audit
void AuditWeapons(const char* filter); // sv_weapon_audit
void AuditBelt(); // sv_belt_audit

// sv_cap_audit -- per hit type, what ceiling this content declares and which
// section is the highest one. ⭐This is the report that unblocked capping the
// non-bullet types: it answers "what can the world legitimately produce" from
// the ini, without waiting for a claim on the wire to describe it.
void AuditCaps();
} // namespace SVDamage
