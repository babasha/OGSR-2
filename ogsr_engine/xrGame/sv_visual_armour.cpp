#include "StdAfx.h"
#include "sv_visual_armour.h"

#include "../xr_3da/fmesh.h"
#include "../Include/xrRender/Kinematics.h"
#include "Level.h"

#include <map>
#include <mutex>

namespace
{
// One entry per visual, including the negative answer: a model with no
// userdata is cached too, so a creature without armour does not re-open its
// .ogf on every bullet.
// ⚠Asked from the main thread (client hit) and the second (server hit);
// entries are never erased while a level runs, find/insert lock.
xr_map<shared_str, SVVisualArmour::Model> s_models;
std::mutex s_models_mx;

// The name a model is stored under: lower-cased, backslashes, no extension, so
// "actors\Dolg\stalker_do_antigas.ogf" and "actors\dolg\stalker_do_antigas"
// are one entry. Same normalisation sv_bone_names does, for the same reason.
shared_str normalize(const shared_str& visual)
{
    if (!visual.size())
        return shared_str();

    string_path buf;
    xr_strcpy(buf, sizeof(buf), visual.c_str());
    for (char* p = buf; *p; ++p)
        if (*p == '/')
            *p = '\\';
    xr_strlwr(buf);

    const size_t n = xr_strlen(buf);
    if (n > 4 && 0 == xr_strcmp(buf + n - 4, ".ogf"))
        buf[n - 4] = 0;

    return shared_str(buf);
}

// Read chunk OGF_S_USERDATA and ask it the two questions CAI_Stalker::net_Spawn
// asks LL_UserData(). The chunk is an ini file -- in this content usually a
// single #include -- so it is parsed by the same CInifile the engine would
// build, with $game_config$ as the include root.
void load(const shared_str& key, SVVisualArmour::Model& out)
{
    string_path fname;
    strconcat(sizeof(fname), fname, key.c_str(), ".ogf");

    string_path full;
    if (!FS.exist(full, "$level$", fname))
        FS.update_path(full, "$game_meshes$", fname);

    IReader* base = FS.r_open(full);
    if (!base)
    {
        Msg("! [sv_armour] cannot open '%s' -- this creature counts as unarmoured", full);
        return;
    }

    if (IReader* ud = base->open_chunk(OGF_S_USERDATA))
    {
        CInifile* ini = xr_new<CInifile>(ud, FS.get_path("$game_config$")->m_Path);

        // ⚠Both lookups are the engine's, key for key. bone_protection is a
        // SECTION with one line in it; immunities is another. A model may
        // declare either, both or neither.
        if (ini->line_exist("bone_protection", "bones_protection_sect"))
        {
            LPCSTR prot = ini->r_string("bone_protection", "bones_protection_sect");
            if (prot && prot[0])
            {
                out.protection = prot;
                out.found = true;
            }
        }
        if (ini->line_exist("immunities", "immunities_sect"))
        {
            LPCSTR imm = ini->r_string("immunities", "immunities_sect");
            if (imm && imm[0])
                out.immunities = imm;
        }

        xr_delete(ini);
        ud->close();
    }
    // No userdata chunk is not an error: most visuals in a level are props.

    FS.r_close(base);
}
} // namespace

const SVVisualArmour::Model& SVVisualArmour::For(const shared_str& visual)
{
    static const Model empty;

    const shared_str key = normalize(visual);
    if (!key.size())
        return empty;

    {
        std::lock_guard<std::mutex> g(s_models_mx);
        auto it = s_models.find(key);
        if (it != s_models.end())
            return it->second;
    }

    Model m;
    load(key, m); // a failure caches the empty answer on purpose: one complaint, not one per hit
    std::lock_guard<std::mutex> g(s_models_mx);
    auto ins = s_models.emplace(key, m);
    return ins.first->second;
}

void SVVisualArmour::Prime(const shared_str& visual, u16 owner)
{
    if (!g_pGameLevel || owner == 0xffff)
        return;
    const shared_str key = normalize(visual);
    if (!key.size())
        return;
    {
        std::lock_guard<std::mutex> g(s_models_mx);
        if (s_models.count(key))
            return;
    }
    // the client object by id: safe from the second thread too -- objects come
    // and go in frame_move, which never runs alongside it
    CObject* O = Level().Objects.net_Find(owner);
    if (!O || !O->Visual() || normalize(O->cNameVisual()) != key)
        return;
    IKinematics* K = O->Visual()->dcast_PKinematics();
    if (!K || !K->LL_BoneCount())
        return; // the null kinematics stub (headless): For() reads the file
    Model m;
    if (CInifile* ini = K->LL_UserData())
    {
        if (ini->line_exist("bone_protection", "bones_protection_sect"))
        {
            LPCSTR prot = ini->r_string("bone_protection", "bones_protection_sect");
            if (prot && prot[0])
            {
                m.protection = prot;
                m.found = true;
            }
        }
        if (ini->line_exist("immunities", "immunities_sect"))
        {
            LPCSTR imm = ini->r_string("immunities", "immunities_sect");
            if (imm && imm[0])
                m.immunities = imm;
        }
    }
    std::lock_guard<std::mutex> g(s_models_mx);
    s_models.emplace(key, m);
}

SVDamage::BoneArmour SVVisualArmour::BoneArmourFor(const shared_str& visual, const shared_str& section, const char* bone_name)
{
    const Model& m = For(visual);
    if (m.found)
    {
        SVDamage::BoneArmour ba = SVDamage::ReadProtection(m.protection.c_str(), bone_name);
        if (ba.found)
            return ba;
    }

    // ⭐Fall back to the section rather than to nothing. No section in this
    // content declares the line, but a mod that puts it where the old code
    // looked keeps working, and a creature whose model went missing still
    // reports whatever its section can offer.
    return SVDamage::BoneArmourFor(section, bone_name);
}

bool SVVisualArmour::ApplyImmunities(const shared_str& visual, float out[])
{
    const Model& m = For(visual);
    if (!m.immunities.size())
        return false;

    return SVDamage::ReadImmunities(m.immunities.c_str(), out);
}

bool SVVisualArmour::Resolve(const char* in, string_path& out)
{
    out[0] = 0;
    if (!in || !in[0])
        return false;

    xr_strcpy(out, sizeof(string_path), in);
    for (char* p = out; *p; ++p)
        if (*p == '/')
            *p = '\\';
    xr_strlwr(out);

    const size_t n = xr_strlen(out);
    if (n > 4 && 0 == xr_strcmp(out + n - 4, ".ogf"))
        out[n - 4] = 0;

    // "meshes\actors\..." is what the .db listing prints, and typing it back in
    // is the obvious thing to do. $game_meshes$ already IS meshes\.
    if (0 == strncmp(out, "meshes\\", 7))
    {
        string_path tmp;
        xr_strcpy(tmp, sizeof(tmp), out + 7);
        xr_strcpy(out, sizeof(string_path), tmp);
    }

    string_path with_ext;
    xr_sprintf(with_ext, "%s.ogf", out);
    if (FS.exist("$game_meshes$", with_ext))
        return true;

    Msg("! [sv_armour] no model '%s' in $game_meshes$ -- refusing to name it in a spawn packet", out);
    Msg("- [sv_armour] list what there is: sv_npc_models [filter], e.g. sv_npc_models actors\\dolg");
    out[0] = 0;
    return false;
}

void SVVisualArmour::Reset()
{
    std::lock_guard<std::mutex> g(s_models_mx);
    s_models.clear();
}

namespace
{
// The two bones a protection set is judged by. Torso is what most bullets
// actually land on and head is what decides whether one shot is enough, so a
// catalogue that prints these two says more than one printing all 45.
const char* const kTorsoBone = "bip01_spine";
const char* const kHeadBone = "bip01_head";

void print_one(const char* label, const SVVisualArmour::Model& m)
{
    if (!m.found)
    {
        Msg("~ [sv_armour]   %-44s UNARMOURED%s", label, m.immunities.size() ? " (immunities only)" : "");
        return;
    }

    const SVDamage::BoneArmour torso = SVDamage::ReadProtection(m.protection.c_str(), kTorsoBone);
    const SVDamage::BoneArmour head = SVDamage::ReadProtection(m.protection.c_str(), kHeadBone);

    Msg("~ [sv_armour]   %-44s %-28s torso %.2f  head %.2f  frac %.3f", label, m.protection.c_str(), torso.armour, head.armour, torso.hit_fraction);
}
} // namespace

void SVVisualArmour::Audit(const char* filter)
{
    if (filter && filter[0])
    {
        const shared_str v(filter);
        const Model& m = For(v);
        Msg("~ [sv_armour] '%s':", filter);
        print_one(normalize(v).c_str(), m);
        if (m.immunities.size())
            Msg("~ [sv_armour]   immunities overridden by the model: %s", m.immunities.c_str());
        if (!m.found && !m.immunities.size())
            Msg("- [sv_armour]   this model declares no userdata -- protection comes from its section, if any");
        return;
    }

    Msg("~ [sv_armour] %u visuals asked about so far", u32(s_models.size()));
    for (const auto& e : s_models)
        print_one(e.first.c_str(), e.second);
}

void SVVisualArmour::Catalogue(const char* filter)
{
    // Read off the filesystem, which sees inside the packed .db archives exactly
    // as the client will when it comes to draw one. A list compiled into the
    // engine would describe the install it was written on and nothing else.
    FS_FileSet found;
    FS.file_list(found, "$game_meshes$", FS_ListFiles, "*.ogf");

    if (found.empty())
    {
        Msg("! [sv_armour] no .ogf visible under $game_meshes$ at all -- is gamedata mounted?");
        return;
    }

    string_path needle{};
    const bool narrowed = filter && filter[0];
    if (narrowed)
    {
        xr_strcpy(needle, sizeof(needle), filter);
        for (char* p = needle; *p; ++p)
            if (*p == '/')
                *p = '\\';
        xr_strlwr(needle);
    }

    // Character models only. Everything else in $game_meshes$ is scenery, and a
    // catalogue of armour that lists doors is a catalogue nobody reads.
    const char* const kActors = "actors\\";

    constexpr u32 kMaxRows = 200;
    u32 matched = 0, shown = 0, armoured = 0;
    for (const FS_File& f : found)
    {
        string_path name;
        xr_strcpy(name, sizeof(name), f.name.c_str());
        for (char* p = name; *p; ++p)
            if (*p == '/')
                *p = '\\';
        xr_strlwr(name);

        if (0 != strncmp(name, kActors, xr_strlen(kActors)))
            continue;
        if (narrowed && !strstr(name, needle))
            continue;

        const size_t n = xr_strlen(name);
        if (n > 4)
            name[n - 4] = 0;

        ++matched;
        const Model& m = For(shared_str(name));
        if (m.found)
            ++armoured;

        if (shown < kMaxRows)
        {
            print_one(name, m);
            ++shown;
        }
    }

    if (!matched)
    {
        Msg("! [sv_armour] no character model matches '%s'", filter ? filter : "");
        Msg("- [sv_armour] try a faction folder: actors\\dolg  actors\\bandit  actors\\military  actors\\stalker_neutral");
        return;
    }

    Msg("~ [sv_armour] %u character models, %u of them armoured by their model", matched, armoured);

    // ⚠Said out loud when the list was cut. A listing that silently stops at a
    // limit reads as "that is all there is".
    if (matched > shown)
        Msg("! [sv_armour] %u more not shown -- narrow with a filter", matched - shown);
}
