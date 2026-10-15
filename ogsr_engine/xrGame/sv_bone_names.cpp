#include "StdAfx.h"
#include "sv_bone_names.h"

#include "../xr_3da/fmesh.h"
#include "../Include/xrRender/Kinematics.h"
#include "Level.h"

#include <map>
#include <mutex>

namespace
{
// One entry per visual. Empty vector = "asked for, and this visual has no
// bones" -- cached too, so a static prop is not re-opened every hit.
// ⚠Asked from two threads: the client's CEntityAlive::Hit (main) and the
// server's hit processing (the second thread). Entries are never erased while
// a level runs, so a reference handed out stays valid; find/insert lock.
using Table = xr_vector<shared_str>;
xr_map<shared_str, Table> s_tables;
std::mutex s_tables_mx;

// The name a bone list is stored under: lower-cased, no extension, so
// "actors\Stalker\stalker.ogf" and "actors\stalker\stalker" are one entry.
shared_str normalize(const shared_str& visual)
{
    if (!visual.size())
        return shared_str();

    string_path buf;
    xr_strcpy(buf, sizeof(buf), visual.c_str());
    xr_strlwr(buf);
    if (const char* dot = strstr(buf, ".ogf"))
        buf[dot - buf] = 0;
    return shared_str(buf);
}

// Read chunk OGF_S_BONE_NAMES and keep the names in file order, because that
// order IS the bone id numbering the wire uses.
bool load(const shared_str& key, Table& out)
{
    string_path fname;
    strconcat(sizeof(fname), fname, key.c_str(), ".ogf");

    string_path full;
    if (!FS.exist(full, "$level$", fname))
        FS.update_path(full, "$game_meshes$", fname);

    IReader* base = FS.r_open(full);
    if (!base)
    {
        Msg("! [sv_bones] cannot open '%s' -- bone ids for this visual stay unresolved", full);
        return false;
    }

    bool ok = false;
    if (base->find_chunk(OGF_S_BONE_NAMES))
    {
        const u32 count = base->r_u32();
        out.reserve(count);
        for (u32 i = 0; i < count; ++i)
        {
            string256 name;
            base->r_stringZ(name, sizeof(name));
            xr_strlwr(name);
            out.emplace_back(name);

            string256 parent;
            base->r_stringZ(parent, sizeof(parent)); // kept only to walk the cursor
            Fobb obb;
            base->r(&obb, sizeof(obb)); // ...and so is this
        }
        ok = true;
    }
    // No such chunk is not an error: most visuals in a level are not skeletons.

    FS.r_close(base);
    return ok;
}

bool copy_names(CObject* O, const shared_str& key, Table& out)
{
    if (!O || !O->Visual() || normalize(O->cNameVisual()) != key)
        return false;
    IKinematics* K = O->Visual()->dcast_PKinematics();
    const u16 count = K ? K->LL_BoneCount() : 0;
    if (!count)
        return false; // the null kinematics stub (headless): read the file
    out.reserve(count);
    for (u16 b = 0; b < count; ++b)
    {
        string256 name;
        xr_strcpy(name, K->LL_BoneName(b));
        xr_strlwr(name);
        out.emplace_back(name);
    }
    return true;
}

// A process that renders has this skeleton loaded already: copy its names.
// 📏07-10-2026 Switch v208/v215: load() is r_open of the WHOLE .ogf (several MB
// out of the archive) for one chunk -- 12-20 ms on the first hit on each new
// visual, on the main thread (CEntityAlive::Hit -> NoteApplied) and on the
// second (the server's hit processing). Same order either way: the skeleton
// numbers its bones in OGF_S_BONE_NAMES order.
// The owner by id from any thread (objects are created and destroyed in
// frame_move, which never runs alongside the second thread); a walk of the
// whole list only on the main thread.
bool from_client(const shared_str& key, u16 owner, Table& out)
{
    if (!g_pGameLevel)
        return false;
    CObjectList& objects = Level().Objects;
    if (owner != 0xffff && copy_names(objects.net_Find(owner), key, out))
        return true;
    if (!Device.OnMainThread())
        return false;
    for (u32 i = 0, n = objects.o_count(); i < n; ++i)
        if (copy_names(objects.o_get_by_iterator(i), key, out))
            return true;
    return false;
}

const Table& table_for(const shared_str& visual, u16 owner = 0xffff)
{
    static const Table empty;

    const shared_str key = normalize(visual);
    if (!key.size())
        return empty;

    {
        std::lock_guard<std::mutex> g(s_tables_mx);
        auto it = s_tables.find(key);
        if (it != s_tables.end())
            return it->second;
    }

    Table t;
    if (!from_client(key, owner, t))
        load(key, t); // a failure caches an empty table on purpose: one complaint, not one per bullet
    std::lock_guard<std::mutex> g(s_tables_mx);
    auto ins = s_tables.emplace(key, std::move(t)); // the other thread may have been first: keeps its entry
    return ins.first->second;
}
} // namespace

const char* SVBones::Name(const shared_str& visual, u16 bone_id, u16 owner)
{
    const Table& t = table_for(visual, owner);
    if (bone_id >= t.size())
        return "";
    return t[bone_id].c_str();
}

u16 SVBones::Count(const shared_str& visual) { return u16(table_for(visual).size()); }

void SVBones::Reset()
{
    std::lock_guard<std::mutex> g(s_tables_mx);
    s_tables.clear();
}

void SVBones::Audit(const char* visual)
{
    if (!visual || !visual[0])
    {
        Msg("~ [sv_bones] %u visuals cached", u32(s_tables.size()));
        for (const auto& e : s_tables)
            Msg("~ [sv_bones]   %s: %u bones", e.first.c_str(), u32(e.second.size()));
        return;
    }

    const shared_str v(visual);
    const u16 n = Count(v);
    Msg("~ [sv_bones] '%s': %u bones", visual, n);
    for (u16 i = 0; i < n; ++i)
        Msg("~ [sv_bones]   %3u %s", i, Name(v, i));
}
