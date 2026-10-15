#pragma once

// Throws the player's bolt without a player.
//
// ⭐WHY this exists rather than asking the user to press a key.
//
// The two symptoms it was built for -- a thrown bolt that produces no bolt, and
// a client that dies in svector<CBlend*,64>::push_back -- were both found by a
// person at a keyboard, and both cost that person a session each time they were
// reproduced. Neither needs a person: the throw is a keypress on an item the
// server can hand out (sv_item_give actor bolt), and the crash showed up the
// second time merely from holding the bolt while moving.
//
// ⭐Like CLFire, this goes through the real chain -- inventory().Activate on the
// bolt slot, then CActor::IR_OnKeyboardPress(kWPN_FIRE). Nothing here calls
// CMissile::Throw directly. A harness that shortcut to Throw() would have
// skipped eThrowStart, and eThrowStart is where the fake missile is asked for,
// which is the thing under investigation.
//
// It reports, per throw: whether the fake missile ever arrived, and how full the
// HUD model's blend lists are -- the number that reaches 64 and kills the client.
namespace CLBolt
{
// Throw `throws` bolts, `gap_ms` apart.
void Start(int throws, u32 gap_ms);
void Stop();
void Status();

// Once per client frame, from CLevel::OnFrame.
void Frame();
} // namespace CLBolt
