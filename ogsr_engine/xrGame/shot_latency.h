#pragma once

class NET_Packet;

// How long a bullet takes to become a line in the server's record, and whether
// what gets recorded is what was fired.
//
// ⭐The measurement problem this solves: the shooter and the recorder are
// different processes, and Level().timeServer() is each one's OWN uptime
// (NET_Client.h: device_timer->GetElapsed_ms()). The same instant reads
// differently in each exe, so subtracting one from the other produces a number
// that looks like a latency and is not one. Both ends stamp CPU::QPC_us()
// instead -- one counter, one epoch (boot), shared by every process on the box.
//
// ⭐Matching a shooter's line to a recorder's line needs no new wire field: the
// impact point in bone space is three floats the shooter computes and the
// server reads back verbatim (SHit::Write_Packet_Cont w_vec3 / r_vec3), so it
// is at once the shot's fingerprint and the evidence that the payload survived
// the trip. Nothing is added to the packet on purpose -- a measurement that
// changes the protocol is measuring a different protocol.
namespace ShotLatency
{
// The parts of a GE_HIT that identify one bullet, read identically at both ends.
struct Sample
{
    u16 victim{0xffff}, attacker{0xffff}, weapon{0xffff}, bone{0}, hit_type{0};
    float power{0.f}, ap{0.f}, impulse{0.f};
    Fvector in_bone{};
    bool aim{false};
};

// Print one line per shot at each stamped point. Off by default: this is a
// measuring instrument, and an automatic weapon would otherwise write ten
// lines a second into a production host's log.
extern int g_shot_trace;

// ---- shooter's side -------------------------------------------------------
// ⭐A round has left a barrel. Counted, never stored: a firefight fires
// thousands, and the useful number is the RATIO to impacts.
//
// ⚠Why it exists: until now the only thing counted was hits, so "they miss a
// lot" was an inference from watching a magazine empty -- not a measurement.
// A hit rate needs a denominator, and this is it. fired - impacts is every
// round that hit the world, a friend, or nothing at all.
void OnFired(u16 shooter_id);

// The bullet has resolved against an object and its GE_HIT is going out.
// Called by whichever process owns the gun: a network client for a player,
// the host for an NPC.
void OnImpact(const Sample& s);

// ---- recorder's side ------------------------------------------------------
// GE_HIT has come off the wire and is being queued (xrServer::Process_event).
// Cursor must be at the victim id; it is restored.
void OnArrived(NET_Packet& P);
// The queued event has come up for processing (game_sv_base GAME_EVENT_ON_HIT),
// before any gate has had a chance to drop it. Cursor as above.
void OnApplied(NET_Packet& P);
// The record for the event most recently passed to OnApplied is now written.
// `relayed` is false for a hit the server dropped instead of delivering -- a
// written record and a delivered hit are not the same event, and reporting
// them as one is exactly the confusion this test exists to remove.
void OnRecorded(bool relayed);

void Report();
void Reset();

// Parse a GE_HIT payload from the cursor and put the cursor back. Shared so
// both ends read the packet the same way.
void ReadHit(NET_Packet& P, Sample& out);
} // namespace ShotLatency
