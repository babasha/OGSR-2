#pragma once

// ⭐The houses of a level and their openings -- windows, and the doors a man
// walks out by -- for the posts round a house the enemy holed up in.
//
// BAKED (the combat AI plan, step 5): once per level, on a worker during the
// load, from the AI map. A house is a connected run of AI-map vertices under a
// roof and walled round; its doors are where the map walks out of it -- so an
// inner doorway, a link between two of its rooms, is no door (06-10 15:31: one
// was, and put two men "beside the door" in the player's front room); its
// windows are rays from its edges that get out under the sky, and stay only if
// some place outside sees in through them -- those places are kept with the
// window as its posts (06-10 20:20: the izba's west "window" nothing outside
// saw into, struck off as blind every fight). Cached by the AI map's GUID under
// $app_data_root$: the second load reads it in a few milliseconds.
//
// LIVE: until the bake is ready, or on a level without one, the old reading in
// the fight -- rays out from the spot at three heights (06-10, the siege).
//
// Both the old siege (stalker_combat_actions.cpp, ai_tactics_v2 0) and the
// commander (ai_commander, ai_tactics_v2 1) read houses through here.
namespace AIHouse
{
constexpr float SAME_OPENING = 1.5f; // mouths this close, read from two spots, are one opening
constexpr float RADIUS = 12.f; // live: roofed ground this near where the house was first read is the house
constexpr float SAME_HOUSE = 15.f; // live: a new spot within this of it, the same house
constexpr u32 OUT_TIME = 60000; // seen out of it no longer than this, he is back in the same house
constexpr float DOORSTEP = 4.f; // out under the sky this near an opening of it: still at the house
constexpr u32 REREAD = 1000; // a house is read at most this often
constexpr float RESPOT = 3.f; // he was seen this far from the spot last read from: read from there too
constexpr u32 MAX_WATCH = 6; // baked: places outside kept per opening

struct SOpening
{
    Fvector dir{}; // outwards
    Fvector mouth{}; // where it leaves the room, at its height
    Fvector inside{}; // a little in through it: what its posts aim at, and must see
    float height{}; // above the floor inside
    float width{};
    bool door{};
    bool knee{}; // open at knee height too: a door if the AI map walks out through it
    // baked: AI-map vertices outside it that see in through it, nearest first
    u8 watches{};
    u32 watch[MAX_WATCH]{};
};

enum ERead : u8
{
    eOutdoors, // no roof over the spot
    eShed, // a roof, but open round: a shed, a porch -- no house to keep
    eIndoors,
};

// The openings of the house round `spot`; `house`: its baked index, -1 when
// read live.
ERead Read(const Fvector& spot, xr_vector<SOpening>& found, int* house = nullptr);
// something within a dozen metres over it
bool Roofed(const Fvector& point);

// ---- the bake
void BakeStart(); // from CLevel::Load_GameSpecific_Before: the cache, else a worker
void BakeStop(); // from CLevel::~CLevel: before the AI map and the collision go
bool Baked(); // ready to read
int HouseAt(u32 vertex); // the baked house this vertex is in; -1 none
int HouseAt(const Fvector& p);
float HouseRadius(int house); // from its middle to its farthest vertex
Fvector HouseOrigin(int house);
void Describe(const Fvector& p); // the house at p and its openings, to the log
void DbgDraw(const Fvector& eye, float range); // ai_dbg_draw 4: every baked house in range

void Log(LPCSTR why, const Fvector& spot, bool indoors, u32 openings, u32 doors, u32 held_s);
void LogOpening(u32 k, const SOpening& o);

// A house kept against an enemy: what has been read of it, from every spot
// he was seen at in it. `TOpening` is an SOpening with its keeper's own state.
template <class TOpening>
struct THouse
{
    u32 built{}; // when it was last read
    u32 version{}; // bumped whenever the openings change: the posts are placed anew
    u32 seen{}; // the level time of the sighting last read for: an older one does not read it again
    u32 start{}; // when keeping this house began
    u32 left{}; // when he was last seen out of it
    Fvector origin{}; // where he was when it began; the house's middle when baked
    Fvector spot{}; // ...and when it was last read
    bool indoors{};
    int baked{-1}; // the baked house, -1 read live
    float radius{RADIUS};
    xr_vector<TOpening> openings;
};

// Under its roof: nobody takes a post or a cover in there while it is kept.
// 📏06-10 15:37 / 15:51: stalkers sat in the izba's entrance hall, a log wall
// from the player, by the stock pick of "the best cover".
template <class TOpening>
bool Inside(const THouse<TOpening>& house, const Fvector& p)
{
    if (!house.indoors)
        return false;
    if (house.baked >= 0)
        return HouseAt(p) == house.baked;
    return _sqr(p.x - house.origin.x) + _sqr(p.z - house.origin.z) < _sqr(RADIUS) && Roofed(Fvector(p).add(Fvector{0.f, .5f, 0.f}));
}

// Reads the house round `spot` into `house`: a new house, the same one from
// another room (what it adds), back in from the porch, or out of it. The
// openings already known keep their state; a new house drops it all.
template <class TOpening>
void Update(THouse<TOpening>& house, const Fvector& spot, bool log)
{
    house.built = Device.dwTimeGlobal;
    xr_vector<SOpening> found;
    int baked = -1;
    const ERead read = Read(spot, found, &baked);

    // out on the porch and back in: the same house, if not long out
    const bool same_place = baked >= 0 || house.baked >= 0 ? baked == house.baked : house.origin.distance_to(spot) < SAME_HOUSE;
    const bool same_house = house.start && same_place && (house.indoors || Device.dwTimeGlobal < house.left + OUT_TIME);
    house.spot = spot;

    // the openings stay as they were: back in, he finds the posts where he left them
    const auto leave = [&] {
        if (!house.indoors)
            return;
        house.indoors = false;
        house.left = Device.dwTimeGlobal;
    };

    if (read == eOutdoors)
    {
        // on the doorstep, at a window from outside: still the house
        const bool at_house = house.indoors && std::any_of(house.openings.begin(), house.openings.end(), [&](const TOpening& o) {
            return _sqr(o.mouth.x - spot.x) + _sqr(o.mouth.z - spot.z) < _sqr(DOORSTEP);
        });
        if (!at_house)
            leave();
        return;
    }
    if (read == eShed)
    {
        leave();
        return;
    }

    const auto doors = [&] { return u32(std::count_if(house.openings.begin(), house.openings.end(), [](const TOpening& o) { return o.door; })); };
    if (!same_house)
    {
        house.indoors = true;
        house.start = Device.dwTimeGlobal;
        house.baked = baked;
        house.origin = baked >= 0 ? HouseOrigin(baked) : spot;
        house.radius = baked >= 0 ? HouseRadius(baked) : RADIUS;
        house.openings.clear();
        for (const SOpening& o : found)
        {
            TOpening kept{};
            static_cast<SOpening&>(kept) = o;
            house.openings.push_back(kept);
        }
        ++house.version;
        if (log)
            Log(baked >= 0 ? "begins (baked)" : "begins", spot, true, u32(house.openings.size()), doors(), 0);
    }
    else
    {
        // the same house from another room, or back in from the porch: what it adds
        const bool back = !house.indoors;
        house.indoors = true;
        u32 added = 0;
        for (const SOpening& opening : found)
        {
            const bool known = std::any_of(house.openings.begin(), house.openings.end(),
                                           [&](const TOpening& o) { return o.mouth.distance_to_sqr(opening.mouth) < _sqr(SAME_OPENING); });
            if (known)
                continue;
            TOpening kept{};
            static_cast<SOpening&>(kept) = opening;
            house.openings.push_back(kept);
            ++added;
        }
        if (added)
            ++house.version;
        if ((added || back) && log)
            Log(back ? "resumes" : "grows", spot, true, u32(house.openings.size()), doors(), (Device.dwTimeGlobal - house.start) / 1000);
        if (!added)
            return;
    }

    if (log)
        for (u32 k = 0; k < house.openings.size(); ++k)
            LogOpening(k, house.openings[k]);
}
} // namespace AIHouse
