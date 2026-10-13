////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_version.h
//	Description : Null-safe accessor for the ALife spawn-format version
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "ai_space.h"
#include "alife_simulator.h"
#include "alife_simulator_header.h"

// Returns the ALife spawn-format version that save/spawn packet (de)serialization
// keys its layout on.
//
// A network client has no local CALifeSimulator: the world (ALife and the game
// graph) is provided by the server, so ai().get_alife() is null BY DESIGN on the
// client. In that case fall back to
// ALIFE_VERSION — in a live session the client and the server are always the same
// build, so the format number the server wrote is exactly this build's
// ALIFE_VERSION.
//
// The has-alife branch keeps reading header().version() verbatim: that number can
// legitimately be LOWER than ALIFE_VERSION when a genuinely old save is loaded
// (spawn-format backward compatibility), and that must never be flattened to the
// current constant.
IC u32 alife_version() { return ai().get_alife() ? ai().get_alife()->header().version() : (u32)ALIFE_VERSION; }
