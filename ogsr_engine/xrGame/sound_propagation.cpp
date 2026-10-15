#include "StdAfx.h"

#include "sound_propagation.h"
#include "ai_space.h"
#include "level_graph.h"

// ⭐snd_prop (08-10-2026). A sound behind a wall was either heard THROUGH it (muffled, from where it is) or not at
// all; a shot in the next room never came in through the door. The AI map already knows every walkable way: a
// breadth-first walk from the listener's node over it, a few times a second, gives each node within
// snd_prop_radius its distance and the way back. A source asks for its node: how long the way is, and the node
// ~2.5 m from the listener the way leaves by -- the door, the corner -- which is where the sound seems to come
// from. The sound side (CSoundRender_Emitter::occ_update) weighs that against what comes through the wall.
//
// The sound thread calls both methods under the sound manager's lock; the AI map is read-only after load and
// everything used here (vertex_id by position, links, vertex positions) is a pure lookup. Reset() goes through
// set_propagation(nullptr), which waits for an update that is still asking.

namespace
{
class CLevelSoundPaths final : public CSound_propagation
{
    struct Slot
    {
        u32 key;
        u32 parent;
        u32 depth;
    };
    static constexpr u32 table_bits = 16;
    static constexpr u32 table_size = 1u << table_bits;
    static constexpr u32 empty = u32(-1);

    const CLevelGraph& G;
    const float cell;
    xr_vector<Slot> table;
    xr_vector<u32> queue;
    u32 used{};
    u32 root{empty};

    static IC u32 hash(u32 key) { return (key * 2654435761u) >> (32 - table_bits); }

    const Slot* find(u32 key) const
    {
        for (u32 h = hash(key);; h = (h + 1) & (table_size - 1))
        {
            const Slot& s = table[h];
            if (s.key == key)
                return &s;
            if (s.key == empty)
                return nullptr;
        }
    }

    Slot* insert(u32 key)
    {
        if (used >= table_size * 7 / 10)
            return nullptr; // full enough: the walk stops here
        for (u32 h = hash(key);; h = (h + 1) & (table_size - 1))
        {
            Slot& s = table[h];
            if (s.key == empty)
            {
                s.key = key;
                used++;
                return &s;
            }
        }
    }

    // the node under P, or under a point one cell off (a source in a wall, the listener on a crate)
    u32 locate(const Fvector& P) const
    {
        u32 id = G.vertex_id(P);
        if (G.valid_vertex_id(id))
            return id;
        const float o[4][2] = {{cell, 0.f}, {-cell, 0.f}, {0.f, cell}, {0.f, -cell}};
        for (const auto& d : o)
        {
            id = G.vertex_id(Fvector{P.x + d[0], P.y, P.z + d[1]});
            if (G.valid_vertex_id(id))
                return id;
        }
        return empty;
    }

public:
    explicit CLevelSoundPaths(const CLevelGraph& g) : G(g), cell(g.header().cell_size())
    {
        table.resize(table_size, Slot{empty, empty, 0});
        queue.reserve(8192);
    }

    void prop_update(const Fvector& listener, float radius) override
    {
        const u32 r = locate(listener);
        if (r == root)
            return; // the walk from this node is current

        root = r;
        std::fill(table.begin(), table.end(), Slot{empty, empty, 0});
        used = 0;
        if (root == empty)
            return;

        // a 4-link grid: the walk's depth is a Manhattan distance, so let it reach the radius on the diagonals too
        const u32 max_depth = u32(radius * 1.3f / cell);
        queue.clear();
        Slot* s0 = insert(root);
        s0->parent = root;
        s0->depth = 0;
        queue.push_back(root);
        for (size_t qi = 0; qi < queue.size(); ++qi)
        {
            const u32 v = queue[qi];
            const u32 d = find(v)->depth;
            if (d >= max_depth)
                continue;
            CLevelGraph::const_iterator i, e;
            G.begin(v, i, e);
            for (; i != e; ++i)
            {
                const u32 n = G.value(v, i);
                if (!G.valid_vertex_id(n) || find(n))
                    continue;
                Slot* s = insert(n);
                if (!s)
                    return;
                s->parent = v;
                s->depth = d + 1;
                queue.push_back(n);
            }
        }
    }

    bool prop_query(const Fvector& P, float& path_len, Fvector& exit_point) override
    {
        if (root == empty)
            return false;
        const u32 v = locate(P);
        if (v == empty || !find(v))
            return false;

        // back along the way to the listener: the length measured every 4th node (the staircase of a 4-link grid
        // is not length), and the node the way leaves the listener's surroundings by
        const u32 exit_depth = _max(1u, u32(2.5f / cell));
        Fvector anchor = G.vertex_position(v);
        float len = P.distance_to(anchor);
        bool exit_set = false;
        u32 cur = v, steps = 0;
        while (cur != root)
        {
            const Slot* c = find(cur);
            if (!c || ++steps > 4096)
                return false;
            if (!exit_set && c->depth <= exit_depth)
            {
                exit_point = G.vertex_position(cur);
                exit_set = true;
            }
            cur = c->parent;
            if (steps % 4 == 0 || cur == root)
            {
                const Fvector p = G.vertex_position(cur);
                len += anchor.distance_to(p);
                anchor = p;
            }
        }
        if (!exit_set)
            exit_point = G.vertex_position(root);
        exit_point.y += 1.2f; // the ear's height over the floor the way runs on
        path_len = len;
        return true;
    }
};

CLevelSoundPaths* g_paths = nullptr;
} // namespace

void SoundPropagation::Frame()
{
    if (g_paths || !::Sound || !ai().get_level_graph())
        return;
    g_paths = xr_new<CLevelSoundPaths>(ai().level_graph());
    ::Sound->set_propagation(g_paths);
    Msg("[snd] propagation: the AI map's paths (%u vertices) handed to the sound", ai().level_graph().header().vertex_count());
}

void SoundPropagation::Reset()
{
    if (!g_paths)
        return;
    if (::Sound)
        ::Sound->set_propagation(nullptr); // waits for a sound update that is still asking
    xr_delete(g_paths);
    Msg("[snd] propagation: taken back from the sound");
}
