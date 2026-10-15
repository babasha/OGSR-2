#pragma once

// ⭐The commander of a fight: who does what against the player, and where. The
// combat AI plan, step 4.
//
// WHY. The roles were claims each man grabbed for himself (g_enemy_claims):
// the first to ask was the suppressor, the nearest free post was his. And a man
// got a claim only from inside the stock action that used it -- the search, the
// detour -- which the stock plan reached through its four flags ("in cover",
// "looked out", "held", "went round"), forced from outside to get there
// (CStalkerCombatPlanner::update). The autotest showed what that gave: the
// siege began in 6 fights of 11, and then late; men stood with no role 20-40 m
// off, cycling take cover -> look out -> hold.
//
// HOW. One commander per enemy, whatever the squads: the village fights as
// several squads, and per-squad bookkeeping let each squad send its own man to
// the door. Each man in the fight reports to it on his own update (main
// thread). It thinks four times a second and at once on an event -- he was
// seen or lost, the house was read anew, a man came or went, a task is done --
// and gives each man a task: a role, a place (asked of the position query, an
// answer the frame after) and where to aim. The plan has two actions for it
// (stalker_combat_task_actions.h): go to the place, fight from it. A man with no
// task fights the stock way; while the enemy is in sight in the open, nobody
// has one.
//
//   in sight, in the open         no tasks: the stock fight
//   lost < SEARCH_DELAY            one suppresses into where he went, up to two go round
//   lost longer                    one goes to look, the others watch round the spot
//   the spot empty                 everybody combs the area, then lets go
//   in a house                     a man in front of each opening, two beside each
//                                  door, the rest round the house; once ringed, one
//                                  of those by the door goes in
//
// ⚠Under ai_tactics_v2 1 only, against the player only, within FIGHT_RANGE of
// him: other fights go the stock way, as everything does under 0. The tasks are
// not saved: after a load they are given anew from what the men remember.
class CAI_Stalker;
struct SStalkerDbgSiege;
enum EStalkerDbgRole : u8;

namespace Commander
{
enum ERole : u8
{
    eRoleNone,
    eRoleSuppress, // fires into where he went to ground, from where he stands
    eRoleFlank, // goes round to a place that sees the spot from off the squad's side
    eRolePost, // in front of an opening of the house, gun in through it
    eRoleDoor, // against the wall beside a door, gun on the doorway
    eRoleWatch, // round the spot or the house, gun on it
    eRoleSearch, // walks up to where he was last seen and looks round there
    eRoleAssault, // from beside the door into the house
    eRoleSweep, // combs the hiding places near the spot
    eRoleCount,
};

struct STask
{
    ERole role{eRoleNone};
    bool place{}; // false: from where he stands
    bool pending{}; // the place is being asked for: he holds, gun on the focus
    u32 vertex{u32(-1)};
    Fvector position{};
    Fvector focus{}; // what his gun is on while he does not see the enemy
    bool stand{}; // at the place: standing -- crouched he sees nothing from it
    bool run{}; // to the place: running, not walking with the gun up
    bool suppress{}; // bursts at the focus without seeing him
    u32 look{}; // at the place: looks round this long (ms), then Done
    bool give_up{}; // the sweep is over: forget him
    bool turn{}; // ...looking round, not only waiting
    u32 serial{}; // changes with every change of the task: the actions start over on it
};

// This man fights under the commander: ai_tactics_v2, his enemy the player,
// within FIGHT_RANGE of him.
bool Applies(const CAI_Stalker& npc);
// From CStalkerCombatPlanner::update, every update of a man it Applies to.
void Report(CAI_Stalker& npc);
// His task against his enemy now; nullptr: none, the stock fight.
const STask* Task(const CAI_Stalker& npc);
// At his task's place, or it has none (or none yet): he fights from here.
bool AtPlace(const CAI_Stalker& npc);
// What the task asked for at the place is done: looked round, waited by the door.
void Done(const CAI_Stalker& npc);
// He gets no nearer his place than where he stands: it is there now.
void Unreachable(const CAI_Stalker& npc);
// "covering fire!" and the like: true once per bout per fight, for the first to ask.
bool FirstToCry(u16 enemy, u32 sound, u32 interval);

// Read by the old code's queries (stalker_combat_actions.h) when the commander
// runs this enemy's fight -- the cover pick, the overlay, the autotest.
bool Owns(u16 enemy);
bool InsideHouse(u16 enemy, const Fvector& p);
bool Suppressor(u16 enemy, u16 npc);
u16 Suppressor(u16 enemy);
bool Flanker(u16 enemy, u16 npc);
u16 Searcher(u16 enemy);
bool DbgHouse(u16 enemy, SStalkerDbgSiege& out);
EStalkerDbgRole DbgRole(u16 enemy, u16 npc, string64& text);
void DbgDrawHouses(const Fvector& eye, float range);
// The autotest: where the fight thinks he is, and when anybody last saw him.
bool DbgSpot(u16 enemy, Fvector& spot, u32& seen_time);
LPCSTR RoleName(ERole role);

void Forget(u16 enemy); // the autotest, between runs
void Reset(); // from CLevel::~CLevel
} // namespace Commander
