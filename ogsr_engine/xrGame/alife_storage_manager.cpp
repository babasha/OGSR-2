////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"

#include <zstd.h>

#include "alife_storage_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "alife_registry_container.h"
#include "xrServer.h"
#include "Level.h"

#include "../xr_3da/IGame_Persistent.h"
#include "../xrCore/fs_load_prefetch.h"   // ThreadCpuMs: wall vs CPU of the steps

// Split of register_object, defined next to it in alife_simulator_base2.cpp.
namespace ALifeRegProf { void Dump(); }

#include "saved_game_wrapper.h"
#include "script_vars_storage.h"

using namespace ALife;

extern string_path g_last_saved_game;

CALifeStorageManager::~CALifeStorageManager() {}

void CALifeStorageManager::save(LPCSTR save_name, bool update_name)
{
    strcpy_s(g_last_saved_game, save_name);

    string_path save;
    strcpy_s(save, m_save_name);
    if (save_name)
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, SAVE_EXTENSION);
    }
    else
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return;
        }
    }

    u32 source_count;
    u32 dest_count;
    void* dest_data;
    {
        CMemoryWriter stream;
        stream.reserve(4 * 1024 * 1024);
        header().save(stream);
        time_manager().save(stream);
        spawns().save(stream);
        objects().save(stream);
        registry().save(stream);

        g_ScriptVars.save(stream);

        source_count = stream.tell();
        const void* source_data = stream.pointer();
        dest_count = ZSTD_compressBound(source_count);
        dest_data = xr_malloc(dest_count);
        dest_count = ZSTD_compress(dest_data, dest_count, source_data, source_count, 1);
        R_ASSERT(!ZSTD_isError(dest_count));
    }

    string_path temp;
    FS.update_path(temp, "$game_saves$", m_save_name);
    IWriter* writer = FS.w_open(temp);
    writer->w_u32(u32(0));
    writer->w_u32(ALIFE_VERSION);

    writer->w_u32(source_count);
    writer->w(dest_data, dest_count);
    xr_free(dest_data);
    FS.w_close(writer);
#ifdef DEBUG
    Msg("* Game %s is successfully saved to file '%s' (%d bytes compressed to %d)", m_save_name, temp, source_count, dest_count + 4);
#else // DEBUG
    Msg("* Game %s is successfully saved to file '%s'", m_save_name, temp);
#endif // DEBUG

    if (!update_name)
        strcpy_s(m_save_name, save);
}

void CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name)
{
    // Half a second of every load sits in this function and has never been split.
    CTimer _t; _t.Start();
    // CPU time of this thread beside the wall time: a step that only got slower on
    // the wall clock waited (the SD, a lock); one whose CPU grew ran slower.
    double cpu0 = LoadPrefetch::ThreadCpuMs(), cpu_spawns = 0, cpu_objects = 0, cpu_idgen = 0;
    auto cpu_lap = [&cpu0] { const double c = LoadPrefetch::ThreadCpuMs(), v = c - cpu0; cpu0 = c; return v; };
    auto lap = [&_t] { const float v = _t.GetElapsed_ms_total(); _t.Start(); return v; };
    float ms_hdr = 0.f, ms_spawns = 0.f, ms_graph = 0.f, ms_objects = 0.f, ms_idgen = 0.f,
          ms_registry = 0.f, ms_script = 0.f, ms_onreg = 0.f;

    IReader source(buffer, buffer_size);
    header().load(source);
    time_manager().load(source);
    ms_hdr = lap();
    cpu_lap();

    spawns().load(source, file_name);
    ms_spawns = lap();
    cpu_spawns = cpu_lap();

    graph().on_load();
    ms_graph = lap();

    objects().load(source);
    ms_objects = lap();
    cpu_objects = cpu_lap();

    restore_lost_shapes();

    VERIFY(can_register_objects());
    can_register_objects(false);
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator B = objects().objects().begin();
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator E = objects().objects().end();
    CALifeObjectRegistry::OBJECT_REGISTRY::iterator I;
    for (I = B; I != E; ++I)
    {
        ALife::_OBJECT_ID id = (*I).second->ID;
        (*I).second->ID = server().PerformIDgen(id);
        VERIFY(id == (*I).second->ID);
        register_object((*I).second, false);
    }
    ms_idgen = lap();
    cpu_idgen = cpu_lap();

    registry().load(source);
    ms_registry = lap();

    g_ScriptVars.load(source);
    ms_script = lap();

    can_register_objects(true);

    for (I = B; I != E; ++I)
        (*I).second->on_register();
    ms_onreg = lap();

    { namespace P = ALifeRegProf; P::Dump(); }
    Msg("[load step]   alife save: header %.0f | spawns %.0f | graph %.0f | objects %.0f (%u) | idgen+register %.0f | registry %.0f | script vars %.0f | on_register %.0f ms",
        ms_hdr, ms_spawns, ms_graph, ms_objects, (u32)objects().objects().size(), ms_idgen, ms_registry, ms_script, ms_onreg);
    Msg("[load step]   alife save CPU: spawns %.0f of %.0f | objects %.0f of %.0f | idgen+register %.0f of %.0f ms (CPU of wall)",
        cpu_spawns, ms_spawns, cpu_objects, ms_objects, cpu_idgen, ms_idgen);
}

// Saves made while CSE_Shape::cform_read checked a box against sizeof(Fmatrix)
// (64) instead of the 48 bytes on the wire hold these objects without the boxes
// that ended their packet -- every ladder, ~900 space restrictors, smart terrains
// -- and a restrictor's type byte read from inside the lost matrix. Both are
// static spawn data, so the spawn registry (re-read from all.spawn on every load)
// is the authority: give back what the save is missing.
void CALifeStorageManager::restore_lost_shapes()
{
    u32 n_fixed = 0;
    for (auto& it : objects().objects())
    {
        CSE_ALifeDynamicObject* object = it.second;
        CSE_Shape* shape = smart_cast<CSE_Shape*>(object);
        if (!shape)
            continue;
        const auto* vertex = spawns().spawns().vertex(object->m_tSpawnID);
        if (!vertex)
            continue;
        CSE_Abstract& spawn = vertex->data()->object();
        CSE_Shape* spawn_shape = smart_cast<CSE_Shape*>(&spawn);
        if (!spawn_shape || spawn_shape->shapes.size() <= shape->shapes.size() || xr_strcmp(spawn.s_name, object->s_name))
            continue;

        shape->shapes = spawn_shape->shapes;
        CSE_ALifeSpaceRestrictor* restrictor = smart_cast<CSE_ALifeSpaceRestrictor*>(object);
        CSE_ALifeSpaceRestrictor* spawn_restrictor = smart_cast<CSE_ALifeSpaceRestrictor*>(&spawn);
        if (restrictor && spawn_restrictor)
            restrictor->m_space_restrictor_type = spawn_restrictor->m_space_restrictor_type;
        if (n_fixed < 8)
            Msg("* [shapes] restored %u shape(s) of '%s' (%s) from the spawn", u32(shape->shapes.size()), object->name_replace(), object->s_name.c_str());
        ++n_fixed;
    }
    if (n_fixed)
        Msg("* [shapes] %u objects of this save had lost their shapes -- restored from all.spawn", n_fixed);
}

bool CALifeStorageManager::load(LPCSTR save_name)
{
    CTimer timer;
    timer.Start();
    string256 save;
    strcpy_s(save, m_save_name);
    if (!save_name)
    {
        if (!xr_strlen(m_save_name))
            R_ASSERT2(false, "There is no file name specified!");
    }
    else
        strconcat(sizeof(m_save_name), m_save_name, save_name, SAVE_EXTENSION);

    strcpy_s(m_loaded_save, sizeof(m_loaded_save) - 1, m_save_name);

    string_path file_name;
    FS.update_path(file_name, "$game_saves$", m_save_name);

    IReader* stream;
    stream = FS.r_open(file_name);
    if (!stream)
    {
        Msg("* Cannot find saved game %s", file_name);
        strcpy_s(m_save_name, save);
        return (false);
    }

    u32 magic{};
    R_ASSERT(CSavedGameWrapper::valid_saved_game(*stream, magic), make_string("%s\nSaved game version mismatch or saved game is corrupted", file_name));

    CTimer _tp; _tp.Start();
    unload();
    reload(m_section);
    const float ms_reload = _tp.GetElapsed_ms_total();

    _tp.Start();
    const double cpu_unzip0 = LoadPrefetch::ThreadCpuMs();
    u32 source_count = stream->r_u32();
    void* source_data = xr_malloc(source_count);
    if (magic == 0)
        R_ASSERT(ZSTD_decompress(source_data, source_count, stream->pointer(), stream->length() - 3 * sizeof(u32)) == source_count);
    else
        rtc_decompress(source_data, source_count, stream->pointer(), stream->length() - 3 * sizeof(u32));
    const float ms_unzip = _tp.GetElapsed_ms_total();
    const double cpu_unzip = LoadPrefetch::ThreadCpuMs() - cpu_unzip0;
    FS.r_close(stream);
    _tp.Start();
    load(source_data, source_count, file_name);
    const float ms_parse = _tp.GetElapsed_ms_total();
    xr_free(source_data);

    _tp.Start();
    groups().on_after_game_load();
    Msg("[load step] alife save '%s': unload+reload %.0f | decompress %.0f (CPU %.0f, %u KB) | parse %.0f | groups %.0f ms",
        m_save_name, ms_reload, ms_unzip, cpu_unzip, source_count >> 10, ms_parse, _tp.GetElapsed_ms_total());

    VERIFY(graph().actor());

    Msg("* Game %s is successfully loaded from file '%s' (%.3fs)", save_name, file_name, timer.GetElapsed_sec());

    return (true);
}

void CALifeStorageManager::save(NET_Packet& net_packet)
{
    prepare_objects_for_save();

    shared_str game_name;
    net_packet.r_stringZ(game_name);
    save(*game_name, !!net_packet.r_u8());
}

void CALifeStorageManager::prepare_objects_for_save()
{
    Level().ClientSend();
    Level().ClientSave();
}
