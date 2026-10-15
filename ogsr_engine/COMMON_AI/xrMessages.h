#pragma once

// CL	== client 2 server message
// SV	== server 2 client message

enum
{
    M_UPDATE = 0, // DUAL: Update state
    M_SPAWN, // DUAL: Spawning, full state

    M_SV_CONFIG_NEW_CLIENT,
    M_SV_CONFIG_GAME,
    M_SV_CONFIG_FINISHED,

    M_EVENT, // Game Event
    M_CLIENTREADY, // Client has finished to load level and are ready to play

    M_CHANGE_LEVEL, // changing level
    M_LOAD_GAME,
    M_RELOAD_GAME,
    M_SAVE_GAME,
    M_SAVE_PACKET,

    M_SWITCH_DISTANCE,
    M_EVENT_PACK, // Pack of M_EVENT

    //-----------------------------------------------------
    M_CLIENT_CONNECT_RESULT,
    M_CLIENT_REQUEST_CONNECTION_DATA,

    M_CHANGE_LEVEL_GAME,
    //-----------------------------------------------------
    M_AUTH_CHALLENGE,
    M_CL_AUTH,
    //-----------------------------------------------------
    M_STATISTIC_UPDATE,
    M_STATISTIC_UPDATE_RESPOND,
    //-----------------------------------------------------
    
    M_CHANGE_SELF_NAME,

    //-----------------------------------------------------
    // Administration over the wire. CL: a console command an authenticated
    // admin typed; SV: what happened. Gated on the role in the join ticket
    // (xrClientData::role) -- an ordinary player sending one is refused and
    // logged, because a message type nobody is supposed to send is worth
    // knowing about.
    //
    // ⚠️ Appended at the END of this enum on purpose: these values are the wire
    // format, and inserting anywhere else renumbers every message after it, so
    // an old client and a new server would disagree about what a packet means.
    M_ADMIN_COMMAND,
    M_ADMIN_REPLY,
};

enum
{
    GE_OWNERSHIP_TAKE, // DUAL: Client request for ownership of an item
    GE_OWNERSHIP_REJECT, // DUAL: Client request ownership rejection
    GE_TRANSFER_AMMO, // DUAL: Take ammo out of weapon for our weapon
    GE_HIT, //
    GE_DIE, //
    GE_ASSIGN_KILLER, //
    GE_DESTROY, // authorative client request for entity-destroy
    GE_TELEPORT_OBJECT,

    GE_ADD_RESTRICTION,
    GE_REMOVE_RESTRICTION,
    GE_REMOVE_ALL_RESTRICTIONS,

    GE_INFO_TRANSFER, // transfer _new_ info on PDA

    GE_TRADE_SELL,
    GE_TRADE_BUY,

    GE_WPN_AMMO_ADD,
    GE_WPN_STATE_CHANGE,

    GE_GRENADE_EXPLODE,

    GE_CHANGE_POS,

    GE_CHANGE_VISUAL,

    GEG_PLAYER_ACTIVATE_SLOT,
    GEG_PLAYER_ITEM2SLOT,
    GEG_PLAYER_ITEM2BELT,
    GEG_PLAYER_ITEM2RUCK,
    GEG_PLAYER_ITEM_EAT,
    //GEG_PLAYER_ITEM_SELL,
    GEG_PLAYER_ACTIVATEARTEFACT,

    GEG_PLAYER_ATTACH_HOLDER,
    GEG_PLAYER_DETACH_HOLDER,
    
    //-------------------------------------
    GE_KILL_SOMEONE,

    GE_FREEZE_OBJECT,
    GE_LAUNCH_ROCKET,

    GE_TRANSFER_TAKE,
    GE_TRANSFER_REJECT,

    // ⭐What a creature the SERVER simulates is holding, sent to everyone else.
    //
    // Nothing in the original game had to say this: single player runs one
    // process, and stock multiplayer has no NPCs. So an NPC's weapon reached
    // clients as an inventory item and stopped there -- the item was replicated,
    // the fact that it was IN HANDS was not, and remote copies drew an unarmed
    // man who was shooting. Measured 28-07-2026 from play.
    //
    // ⚠Appended at the END of the enum on purpose: these values go on the wire
    // by ordinal, so inserting anywhere else renumbers every event after it.
    GE_NPC_ACTIVE_ITEM,

    // ⭐A creature the SERVER simulates pulled the trigger.
    //
    // Same shaped hole as the one above, one level deeper. The shot itself is
    // fully replicated already -- the bullet flies on the server, the hit
    // arrives as GE_HIT, the victim dies -- but every VISIBLE part of it
    // (muzzle flash, report, tracer, smoke) is produced locally by the weapon
    // that fired, and on a remote client that weapon's trigger is never
    // pulled. Play on 28-07-2026: "не целятся, выстрелов нет, умирают на
    // расстоянии" over a fight the server's telemetry showed in full detail.
    //
    // Carries the fire point and direction the server actually used rather
    // than letting each client guess: the shooter's AI is on the server, and
    // a witness whose tracer disagrees with the corpse is worse than none.
    GE_NPC_SHOT,

    // ⭐An anomaly changed SIZE, said to everyone already looking at it.
    //
    // How far a zone reaches is a sphere in its shape list, and that list only
    // ever travels inside the spawn packet (CSE_Shape::cform_write). That is
    // the right home for it -- it is the object's definition, not its state --
    // but it means an operator resizing a live anomaly has no way to tell the
    // clients that already hold a copy: they were told once, at spawn, and the
    // protocol has no second sentence. The alternative, destroying and
    // respawning the entity, changes its id and makes it blink out of the world
    // in front of whoever is standing in it.
    //
    // The server writes the new radius into the entity as well, so a client
    // that joins afterwards learns it the ordinary way and the two paths cannot
    // drift apart.
    //
    // ⚠Appended at the END, like the two above: these values go on the wire by
    // ordinal, so inserting anywhere else renumbers every event after it -- and
    // renumbering here means a client and a server built minutes apart quietly
    // disagree about what every event means.
    GE_ZONE_RESHAPE,

    // ⭐A placed object was moved or turned, said to everyone already looking
    // at it.
    //
    // Exactly the hole GE_ZONE_RESHAPE fills, one object class over. Where a
    // thing STANDS travels in CSE_Abstract::Spawn_Write and nowhere else, so a
    // prop an operator drags a metre to the left is at the new place for the
    // next joiner and at the old one for everybody in the room. Furniture is
    // edited by nudging -- place, look, turn, nudge, look again -- and an
    // editor whose every adjustment requires a respawn is not an editor.
    //
    // Carries the full pose, position and Euler angles both, because a client
    // that missed one event must not be able to accumulate an error: every
    // message states where the thing IS rather than how far it moved.
    //
    // The server writes the pose into the entity as well, so a client that
    // joins afterwards learns it the ordinary way and the two paths cannot
    // drift apart.
    //
    // ⚠Appended at the END, like the three above: these values go on the wire
    // by ordinal, so inserting anywhere else renumbers every event after it --
    // and renumbering here means a client and a server built minutes apart
    // quietly disagree about what every event means.
    GE_PROP_TRANSFORM,

    // ⭐A creature was RE-DRESSED -- and in this content that means it was
    // re-armoured.
    //
    // bones_protection_sect is declared by a model's .ogf userdata and by no
    // creature section here (sv_visual_armour.h), so "what a stalker wears" and
    // "how hard it is to kill" are one decision. The server can change the
    // entity's visual_name whenever it likes, but that field only ever travels
    // inside the spawn packet (CSE_Visual::visual_write) -- so a creature the
    // clients are already looking at keeps its old body while the server
    // computes damage against the new one. Exactly the hole GE_ZONE_RESHAPE and
    // GE_PROP_TRANSFORM fill for their object classes.
    //
    // Carries the model name in full rather than an index into anything: the
    // catalogue is the filesystem, and a client that joined later must be able
    // to arrive at the same body from the spawn packet alone.
    //
    // ⚠Appended at the END, like the four above: these values go on the wire by
    // ordinal, so inserting anywhere else renumbers every event after it -- and
    // a client and a server built minutes apart then quietly disagree about
    // what every event means.
    GE_NPC_VISUAL,

    // ⭐The server put somebody back on their feet.
    //
    // Nothing in the original game ever had to: single player reloads a save
    // and stock multiplayer respawns you into a fresh body. A persistent world
    // has neither -- the body IS the character, with its inventory and its
    // place in the world -- so death has to be recoverable without destroying
    // and re-creating the entity, which would change its id and lose everything
    // hanging off it.
    //
    // ⚠Death is not a flag, it is a STATE spread across three layers: the
    // condition (health at zero), the entity (level death time, which is what
    // AlreadyDie reads), and the physics (a ragdoll shell instead of a walking
    // capsule). Sending "you are alive again" means undoing all three on the
    // machine that owns the body, which is why this is an event to the owner
    // rather than a health number in an update.
    //
    // ⚠Appended at the END, like the five above: these values go on the wire by
    // ordinal, so inserting anywhere else renumbers every event after it.
    GE_REVIVE,

    GE_FORCEDWORD = u32(-1)
};

enum EGameMessages
{ // game_cl <----> game_sv messages

    GAME_EVENT_PLAYER_CONNECTED,

    GAME_EVENT_CREATE_CLIENT,
    GAME_EVENT_ON_HIT,

    GAME_EVENT_PLAYER_AUTH,
    
    GAME_EVENT_FORCEDWORD = u32(-1)
};

enum
{
    M_SPAWN_OBJECT_LOCAL = (1 << 0), // after spawn it becomes local (authorative)
    M_SPAWN_OBJECT_HASUPDATE = (1 << 2), // after spawn info it has update inside message
    M_SPAWN_OBJECT_ASPLAYER = (1 << 3), // after spawn it must become viewable
    M_SPAWN_OBJECT_PHANTOM = (1 << 4), // after spawn it must become viewable
    M_SPAWN_VERSION = (1 << 5), // control version
    M_SPAWN_UPDATE = (1 << 6), // + update packet
    M_SPAWN_TIME = (1 << 7), // + spawn time
    M_SPAWN_DENIED = (1 << 8), // don't spawn entity with this flag

    M_SPAWN_OBJECT_FORCEDWORD = u32(-1)
};
