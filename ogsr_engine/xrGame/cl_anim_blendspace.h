#pragma once

#include "../Include/xrRender/KinematicsAnimated.h"

// ⭐⭐⭐CONTINUOUS LOCOMOTION FOR A BODY DRIVEN FROM OUTSIDE.
//
// The puppet layer used to pick ONE cycle from a speed band and PlayCycle it.
// Every threshold crossing was therefore a restart, and a restart is what
// "дёрганое" is made of -- measured at 48 idle<->walk restarts a minute around
// the walking threshold. Two rounds of hysteresis and a 250 ms debounce made
// that rarer without making it continuous: the animation still knew only three
// speeds, so anything between them was covered by scaling the playback rate up
// to 70% away from what the clip was authored for, which is the sliding.
//
// A blend space removes the class rather than the instances. Idle, walk and run
// are held on the skeleton AT ONCE, with weights that follow the speed
// continuously, so there is nothing left to restart and nothing left to
// debounce. It needs three things the engine did not previously offer:
//
//   1. blends whose weight is an INPUT              -> CBlend::eManual
//   2. one shared, normalized PHASE across the mix  -> driven here
//   3. how fast each clip claims to move, in m/s    -> measured from the data
//
// (3) is the one worth spelling out. The playback rate that stops the feet from
// sliding is desired_speed / clip_speed, and clip_speed was a constant copied
// out of an ltx by hand -- twice, and wrong the first time by 20%. The clip
// itself knows: root translation over the cycle, divided by the cycle length.
// So measure it, and fall back to the copied constant only when the content
// turns out to animate in place (which X-Ray content usually does -- the AI
// carries the body and the cycle only moves the feet). Either way the number is
// stated out loud rather than assumed: see cl_anim_rootmotion.
namespace ClAnim
{
// What a cycle claims about its own motion. `measured` distinguishes "the root
// covers no ground" from "we could not read the keys" -- the two look identical
// in the resulting speed and mean opposite things about whose fault a slide is.
struct SCycleMetrics
{
    float dist_m{0.f}; // ground covered by the root over one cycle
    float time_s{0.f}; // ...and how long that cycle takes at authored speed
    u32 keys{0};
    bool measured{false}; // the root motion was readable
    bool in_place{false}; // ...and it says the clip does not travel

    float native_speed() const { return (time_s > EPS && dist_m > EPS) ? dist_m / time_s : 0.f; }
};

bool CycleMetrics(IKinematicsAnimated* ka, MotionID m, SCycleMetrics& out);

// ⭐⭐ONE PLACE DECIDES WHAT A GAIT IS WORTH, AND BOTH SIDES ASK IT.
//
// The client needs it to pair a cycle with a playback rate. The server needs
// the same number to decide how fast to actually carry a creature it is
// driving -- and if the two disagree, the feet slide by exactly the difference.
// That is the "two authorities over one value" fault this build has paid for
// six times, so there is one function and no second copy.
//
// `state_class` is the cycle prefix (0 norm_, 1 cr_, 2 dmg_norm_), `mental` is
// npc_mental_*. Returns false when the section names no velocity table.
//
// ⚠It is not a flat lookup: CStalkerVelocityCollection exists only for some
// [mental][body][type][direction] combinations, and the routing inside is what
// keeps a release build from reading a neighbouring cell.
bool GaitSpeeds(LPCSTR section, u8 state_class, u8 mental, bool backwards, float& walk, float& run);

// Console hook: print what the content actually says about its locomotion set,
// for one entity. ⭐The whole point is to print the INPUTS before anyone
// reasons about the output -- the fallback constants in this file were a guess
// that survived two sessions because nothing ever printed the alternative.
void DumpRootMotion(u16 id);

// One instance per puppet. Owns a few blends on the legs partition and nothing
// else; the torso, head and aim layers are untouched and keep working the way
// they do today.
class CLegsBlendSpace
{
public:
    struct SInput
    {
        float speed{0.f}; // ground speed the model is actually covering, m/s
        float fwd{0.f}; // ...signed along the body's facing (for backpedalling)
        u8 state_class{0}; // 0 = norm_, 1 = cr_, 2 = dmg_norm_
        // ⭐npc_mental_free / danger / panic. It picks no cycle -- there are
        // only two -- but it picks the SPEED those two are paired with, and
        // panic is where the sprint lives. See gait_speeds.
        u8 mental{0};
        // ⭐The body is being carried at the sprint tariff whatever its brain
        // is feeling -- the server said so on its own bit. Same clip, higher
        // pairing; see SNetCreatureIntent::sSprint.
        bool sprint{false};
        bool actor{false}; // a remote player's body: same cycles, no crouch set
    };

    void Update(IKinematicsAnimated* ka, LPCSTR diag_sect, u16 diag_id, const SInput& in, float dt);
    void Release(IKinematicsAnimated* ka);
    bool built() const { return m_built; }

private:
    enum
    {
        n_idle = 0,
        n_move_a, // walk, in whichever direction is selected
        n_move_b, // run
        n_nodes
    };

    struct SNode
    {
        CBlend* B{nullptr};
        MotionID motion;
        SCycleMetrics metrics;
        float native{0.f}; // m/s this clip travels at, measured or assumed
        float authored_speed{1.f}; // the playback rate the engine gave it
        float weight{0.f};
    };

    bool Build(IKinematicsAnimated* ka, LPCSTR diag_sect, u16 diag_id, u8 state_class, u8 mental, bool sprint, bool actor, bool back);
    bool NodesAlive() const;

    SNode m_node[n_nodes];
    u16 m_part{u16(-1)};
    u8 m_state_class{0xff};
    u8 m_mental{0xff}; // the tariff row currently in force; re-read in place
    // ⭐Which take of each gait is playing: _1 is the relaxed set, _0 the alert
    // one. This IS part of the cycle names, so a change here is a rebuild --
    // unlike the tariff above, which is a float and is written in place.
    s8 m_idle_var{-1};
    s8 m_move_var{-1};
    bool m_sprint{false};
    bool m_back{false}; // the selected set walks backwards
    bool m_actor{false};
    bool m_built{false};
    float m_phase{0.f}; // shared normalized cycle position, [0,1)
    u32 m_next_retry{0}; // a visual with no usable cycles must not be retried per frame
};
} // namespace ClAnim
