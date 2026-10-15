#pragma once

// ⭐The threat map: where the enemy may be, as a probability over the AI map,
// one map per enemy for the whole fight. The combat AI plan, step 2.
//
// WHY. "Where he is" was one point: the freshest thing anybody in the fight
// remembered of him. Every sound of his put it exactly where he stood -- a
// stalker who never saw him knew his spot to the metre -- and every sound moved
// it; a watcher's place went with it, and the searcher walked to wherever he
// was last heard, found nobody and gave up on the whole area. 📏06-10: up to
// 2042 position queries in one fight, most of them "the spot moved".
//
// HOW (Isla's occupancy map, on the AI map). Seen: the map is the one vertex he
// stands on. Not seen: what he may have done spreads over the vertices he can
// walk to, at about a man's pace -- through doors, not through walls. A vertex
// some man of the fight looks at and finds empty loses most of what it held;
// the rest of the map takes it up. A sound of his moves the likelihood towards
// where it sounded, the farther from the man who heard it the vaguer -- it no
// longer puts him there. What spreads beyond the map's cap is kept as "outside":
// how likely he is not near any more.
//
// The map is stepped on the second thread, four times a second, beside the
// position query; the main thread hands it what happened and reads what the
// last step published. Rays: static geometry only, a few dozen a step.
//
// ⚠Under ai_tactics_v2 1 and ai_threat_map 1 only: the commander's fights. Not
// saved: after a load it is seeded again from what the men remember.
class CAI_Stalker;

namespace ThreatMap
{
struct SEstimate
{
    bool valid{};
    Fvector position{}; // the likeliest place, at his feet
    u32 vertex{u32(-1)};
    float near_mass{}; // how likely he is within a few metres of it
    float spread{}; // the radius that holds most of what is likely
    float outside{}; // how likely he is off the map: gone
    int house{-1}; // the baked house most likely to hold him (ai_house.h), and how likely
    float house_mass{};
    Fvector house_point{}; // ...its likeliest place
    u32 info_time{}; // the last sight or sound folded in
    u32 seen_time{}; // the last sight
};

// ---- what happened, from the main thread
// Somebody's eyes on him (or the memory of it, `time` when it was).
void Seen(u16 enemy, const Fvector& position, u32 vertex, u32 time);
// A sound of his, where it sounded, heard by a man standing at `listener`.
void Heard(u16 enemy, const Fvector& listener, const Fvector& position);
// No map yet: begin one from what a man remembers, `sigma` metres vague.
void Seed(u16 enemy, const Fvector& position, u32 vertex, float sigma, u32 time);
// This man of the fight looks: his eyes clear what they see of the map.
void Look(u16 enemy, const CAI_Stalker& npc);

// ---- what the last step found
bool Exists(u16 enemy);
bool Estimate(u16 enemy, SEstimate& out);
// How likely he is in this baked house.
float HouseMass(u16 enemy, int house);
// The likeliest places, apart from one another, likeliest first.
void Likeliest(u16 enemy, xr_vector<Fvector>& out);

void Forget(u16 enemy); // the fight is over: at the next Frame
void Frame(); // from CLevel::OnFrame, after the AI: publishes the last step, hands out the next
void Reset(); // from CLevel::~CLevel
void DbgDraw(const Fvector& eye, float range); // ai_dbg_draw 16
void Dump(); // ai_pq_stats

struct SStats
{
    u32 steps{}, rays{}, heard{}, seen{}, cells_max{};
    float worker_ms{}, step_max_ms{};
};
const SStats& Stats();
void ResetStats();

extern int g_on; // ai_threat_map
bool On(); // ai_tactics_v2 and ai_threat_map
} // namespace ThreatMap
