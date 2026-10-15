#include "StdAfx.h"
#include "cl_bolt_test.h"

#include "Level.h"
#include "Actor.h"
#include "Inventory.h"
#include "HudItem.h"
#include "Missile.h"
#include "player_hud.h"
#include "xr_level_controller.h"
#include "../Include/xrRender/KinematicsAnimated.h"

namespace
{
enum EState
{
    stOff = 0,
    stDraw,    // ask for the bolt slot and wait for it to be in hand
    stPress,   // trigger down
    stRelease, // trigger up
    stGap,     // wait, then go again
    stDone
};

EState s_state = stOff;
int s_want = 0;
int s_thrown = 0;
u32 s_gap_ms = 1500;
u32 s_next_ms = 0;
u32 s_waited_frames = 0;
u32 s_last_draw_ms = 0;

// Loud enough to notice, rare enough not to drown the log it shares with the
// throw chain's own lines.
constexpr u32 WAIT_WARN_FRAMES = 180;

CActor* actor()
{
    if (!g_pGameLevel)
        return nullptr;
    return smart_cast<CActor*>(Level().CurrentControlEntity());
}

// How full the HUD blend lists are. This is the number that kills the client:
// blend_cycles[part] is a fixed 64 (MAX_BLENDED * MAX_CHANNELS) and its
// push_back asserts. Printing the high-water mark per throw turns "it crashed
// after a while" into a curve.
void report_blends(const char* when)
{
    if (!g_player_hud)
        return;

    u32 hands_max = 0, item_max = 0;

    if (IKinematicsAnimated* K = g_player_hud->Model())
    {
        const u16 pc = K->partitions().count();
        for (u16 p = 0; p < pc; ++p)
            hands_max = _max(hands_max, K->LL_PartBlendsCount(p));
    }

    if (attachable_hud_item* it = g_player_hud->attached_item(0))
    {
        if (it->m_model)
        {
            if (IKinematicsAnimated* KI = it->m_model->dcast_PKinematicsAnimated())
            {
                const u16 pc = KI->partitions().count();
                for (u16 p = 0; p < pc; ++p)
                    item_max = _max(item_max, KI->LL_PartBlendsCount(p));
            }
        }
    }

    Msg("~ [cl_bolt] %s: blends -- hands %u/64, item %u/64", when, hands_max, item_max);
}
} // namespace

void CLBolt::Start(int throws, u32 gap_ms)
{
    s_want = _max(1, throws);
    s_gap_ms = gap_ms ? gap_ms : 1500;
    s_thrown = 0;
    s_waited_frames = 0;
    s_state = stDraw;
    Msg("~ [cl_bolt] armed: %d throws, %u ms apart", s_want, s_gap_ms);
}

void CLBolt::Stop()
{
    if (CActor* A = actor())
        A->IR_OnKeyboardRelease(kWPN_FIRE);
    s_state = stOff;
    Msg("~ [cl_bolt] stopped after %d throws", s_thrown);
}

void CLBolt::Status()
{
    Msg("~ [cl_bolt] state=%d thrown=%d/%d", int(s_state), s_thrown, s_want);
    report_blends("status");
}

void CLBolt::Frame()
{
    // Command-line arming, same shape as -net_move_test and -net_fire_test: the
    // client this runs on is started by a script with nothing to type into.
    static bool s_cmdline_checked = false;
    static u32 s_arm_at_ms = 0;
    if (!s_cmdline_checked)
    {
        s_cmdline_checked = true;
        if (const char* p = strstr(Core.Params, "-net_bolt_test"))
        {
            int throws = 5, delay_s = 25;
            u32 gap = 1500;
            sscanf(p + xr_strlen("-net_bolt_test"), "%d %d %u", &throws, &delay_s, &gap);
            s_want = throws;
            s_gap_ms = gap;
            s_arm_at_ms = Device.dwTimeGlobal + u32(_max(0, delay_s) * 1000);
            Msg("~ [cl_bolt] -net_bolt_test: %d throws, first in %d s, %u ms apart", throws, delay_s, gap);
        }
    }
    if (s_arm_at_ms && Device.dwTimeGlobal >= s_arm_at_ms)
    {
        s_arm_at_ms = 0;
        Start(s_want, s_gap_ms);
    }

    if (s_state == stOff || s_state == stDone)
        return;

    CActor* A = actor();
    if (!A || !A->g_Alive())
        return;

    switch (s_state)
    {
    case stDraw: {
        ++s_waited_frames;

        CMissile* M = smart_cast<CMissile*>(A->inventory().ActiveItem());
        if (M && !M->IsPending() && M->GetState() == CHUDState::eIdle)
        {
            report_blends("before throw");
            Msg("~ [cl_bolt] throw %d/%d: '%s' id=%u in hand, pressing fire", s_thrown + 1, s_want, M->cNameSect().c_str(), M->ID());
            A->IR_OnKeyboardPress(kWPN_FIRE);
            s_state = stPress;
            return;
        }

        // Not in hand yet. Ask for the bolt slot, at most twice a second --
        // drawing it is an animation, and re-asking every frame restarts it.
        if (!M && Device.dwTimeGlobal - s_last_draw_ms >= 500)
        {
            s_last_draw_ms = Device.dwTimeGlobal;
            if (A->inventory().ItemFromSlot(BOLT_SLOT))
                A->inventory().Activate(BOLT_SLOT);
        }

        if ((s_waited_frames % WAIT_WARN_FRAMES) == 0)
        {
            CInventoryItem* slot_item = A->inventory().ItemFromSlot(BOLT_SLOT);
            Msg("! [cl_bolt] still waiting: bolt in slot=%s, active=%s, state=%d (%u frames) -- 'sv_item_give actor bolt' if the slot is empty",
                slot_item ? "yes" : "no", M ? M->cNameSect().c_str() : "not a missile", M ? int(M->GetState()) : -1, s_waited_frames);
        }
        return;
    }

    case stPress: {
        // Held one frame, as CLFire does: enough for the state machine to enter
        // eThrowStart, which is where the fake missile is asked for.
        A->IR_OnKeyboardRelease(kWPN_FIRE);
        ++s_thrown;
        s_next_ms = Device.dwTimeGlobal + s_gap_ms;
        s_state = stRelease;
        return;
    }

    case stRelease: {
        s_state = stGap;
        return;
    }

    case stGap: {
        if (Device.dwTimeGlobal < s_next_ms)
            return;

        report_blends("after throw");

        if (s_thrown >= s_want)
        {
            Msg("~ [cl_bolt] done: %d throws asked for", s_thrown);
            s_state = stDone;
            return;
        }
        s_waited_frames = 0;
        s_state = stDraw;
        return;
    }

    default: return;
    }
}
