#pragma once

// ⭐The position query: ONE way for a stalker to pick a place, whatever for --
// a cover from the enemy, a spot to watch where he went to ground, a post that
// sees in through a window. The combat AI plan, step 3 (an EQS of our own).
//
// WHY. Four pieces of code chose points, each with its own rules: the stock
// "best cover" (how hidden, nothing else), the watchers' sectors (a cover near a
// point on a ring), the siege posts (two loops of their own), the men beside the
// door. Each rule found in a fight had to be taught to each of them, and was
// not: a watcher crouched facing a wall, a post 20 m off behind the wrong fence,
// a cover inside the house the player was in. Here a rule is a test, and every
// kind of place runs the tests that apply to it.
//
// HOW. The caller fills a request on the main thread: the candidates (cover
// points and AI-map vertices near the place, already checked as reachable for
// this man -- that needs his restrictions, which only the main thread may read)
// and what the place is for. The scoring runs on the second thread, alongside
// the render, where nothing the AI owns is in use: the cheap tests on every
// candidate, then rays -- static geometry only, which is safe off the main
// thread -- in order of the cheap score, stopping once a few have passed. A
// budget per frame (ai_pq_budget_ms) caps it; what does not fit goes on next
// frame. The answer is there to Poll the frame after it was asked for.
//
// ⚠Only under ai_tactics_v2 1. The old pickers stay the default until the
// autotest (ai_test siege_izba) says the new one is better.
class CCoverPoint;
class CAI_Stalker;

namespace PositionQuery
{
enum EKind : u8
{
    eCover, // hidden from the threat, able to look out at it
    eWatch, // keeps a spot in sight, round it, apart from the others
    ePost, // sees in through an opening of a house, from in front of it
    eFlank, // keeps the spot in sight from off the squad's side of it (focus_dir: towards the squad)
    eDoor, // against the wall beside a door: hidden from inside (threat), the doorway (focus) in sight
    eSweep, // a hiding place near the spot, as hidden from where the squad came (threat) as can be; no rays
    eKindCount,
};

struct SPoint
{
    Fvector position{};
    u32 vertex{u32(-1)};
    const CCoverPoint* cover{}; // null: bare ground
};

struct SRequest
{
    EKind kind{eCover};
    u16 npc{u16(-1)};
    Fvector self{}; // where he stands now
    Fvector threat{}; // where the enemy is thought to be, at his feet
    Fvector focus{}; // what the place must see: the spot, or a point in through the opening
    Fvector focus_dir{}; // ePost: the opening's outward direction; eFlank: from the spot towards the squad
    Fvector focus_floor{}; // ePost: the opening at floor height outside
    float near_{}, ideal{}, far_{}; // distance from the focus (eCover: from the threat)
    float front{1.f}; // ePost: at least this far out in front of the opening's wall
    bool eyes{}; // ePost: seeing in is enough, from the eyes -- not the muzzle
    u32 looks{16}; // candidates looked at with rays, at most
    // the house the enemy is besieged in: no place under its roof
    bool house{};
    Fvector house_origin{};
    float house_radius{};
    int house_baked{-1}; // the baked house (ai_house.h): its vertices, no roof ray
    // where the others of the fight are, or are going: keep apart
    xr_vector<Fvector> friends;
    float spacing{3.f};
    xr_vector<SPoint> candidates;
};

struct SResult
{
    bool found{};
    SPoint point;
    float score{};
    bool stand{}; // crouched it does not see: keep it standing
    string32 why{}; // what threw the candidates out, by verdict: "d12 v5 n2" -- for the logs
};

// Asks; a request of the same man and kind still waiting is replaced.
void Issue(SRequest&& request);
// The answer, once: true when there is one (then `out` holds it).
bool Poll(u16 npc, EKind kind, SResult& out);
// Asked and not answered yet.
bool Pending(u16 npc, EKind kind);
void Cancel(u16 npc);
void Cancel(u16 npc, EKind kind);

// Candidates, on the main thread: cover points within `radius` of `center`
// this man may use, those nearest `prefer` metres from `center` first; AI-map vertices on rings
// round `center` he may reach.
void GatherCovers(SRequest& r, CAI_Stalker& npc, const Fvector& center, float radius, u32 most, float prefer = 0.f);
void GatherRings(SRequest& r, CAI_Stalker& npc, const Fvector& center, std::initializer_list<float> radii, u32 per_ring);
void GatherPoint(SRequest& r, CAI_Stalker& npc, const Fvector& at);

void Frame(); // from CLevel::OnFrame, after the AI: hands this frame's work to the second thread
void Reset(); // from CLevel::~CLevel: every request names covers of that level
void Dump(); // ai_pq_stats

// Level time this man last asked this kind; 0 never. For the callers' own
// limits on how often to ask (the query has none of its own).
u32 LastAsked(u16 npc, EKind kind);

struct SStats
{
    u32 issued{}, answered{}, found{}, rays{};
    u32 issued_kind[eKindCount]{};
    float main_ms{}; // gathering, on the main thread, all of it
    float worker_ms{}; // scoring, on the second thread, all of it
    float worker_frame_max_ms{}; // the most in one frame
    u32 frames_waited_max{};
};
const SStats& Stats();
void ResetStats();

extern int g_ai_tactics_v2;
extern float g_budget_ms;
} // namespace PositionQuery
