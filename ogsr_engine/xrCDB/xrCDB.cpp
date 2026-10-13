#include "StdAfx.h"

#include "xrCDB.h"
#include "xrCDB_tiled.h"

#include <cstdio>
#include <algorithm>

using namespace CDB;
using namespace Opcode;

MODEL::~MODEL()
{
    syncronize(); // maybe model still in building
    status = S_INIT;
    xr_delete(tiles_); // waits out an in-flight prefetch; references tris/verts — free first
    xr_delete(tree);
    if (storage_view)
    {
        // Loaded from the tiled cache: one mapped view, not two allocations. The
        // warm-up walking it has been joined by ~TILE_GRID above.
        CDB::UnmapStorageView(storage_view);
        storage_view = nullptr;
        tris = nullptr;
        verts = nullptr;
    }
    else
    {
        xr_free(tris);
        xr_free(verts);
    }
    tris_count = 0;
    verts_count = 0;
}

void MODEL::build(const Fvector* V, const size_t Vcnt, const TRI* T, const size_t Tcnt, build_callback* bc, void* bcp)
{
    R_ASSERT(S_INIT == status);
    R_ASSERT((Vcnt >= 4) && (Tcnt >= 2));

    build_internal(V, Vcnt, T, Tcnt, bc, bcp);
    status = S_READY;
}

void MODEL::build_internal(const Fvector* V, const size_t Vcnt, const TRI* T, const size_t Tcnt, build_callback* bc, void* bcp)
{
    // verts
    verts_count = Vcnt;
    verts = xr_alloc<Fvector>(verts_count);
    std::memcpy(verts, V, verts_count * sizeof(Fvector));

    // tris
    tris_count = Tcnt;
    tris = xr_alloc<TRI>(tris_count);
    std::memcpy(tris, T, tris_count * sizeof(TRI));

    // callback
    if (bc)
        bc(verts, Vcnt, tris, Tcnt, bcp);

    // Release data pointers
    status = S_BUILD;
    MeshInterface* mif = xr_new<MeshInterface>();
    mif->SetNbTriangles(tris_count);
    mif->SetNbVertices(verts_count);
    mif->SetPointers(reinterpret_cast<const IceMaths::IndexedTriangle*>(tris), reinterpret_cast<const IceMaths::Point*>(verts));
    mif->SetStrides(sizeof(TRI));

    // Build a non quantized no-leaf tree
    OPCODECREATE OPCC = OPCODECREATE();
    OPCC.mIMesh = mif;
    OPCC.mQuantized = false;

    tree = xr_new<Model>();

    if (!tree->Build(OPCC))
    {
        xr_free(verts);
        xr_free(tris);
        xr_delete(mif);
        return;
    }

    xr_delete(mif);
}

u32 MODEL::memory()
{
    if (S_BUILD == status)
    {
        Msg("! xrCDB: model still isn't ready");
        return 0;
    }
    const u32 V = verts_count * sizeof(Fvector);
    const u32 T = tris_count * sizeof(TRI);
    if (tiles_)
        return u32(tiles_->resident_bytes.load(std::memory_order_relaxed) + (size_t)tiles_->nx * tiles_->nz * sizeof(TILE)) + V + T + sizeof(*this);
    return tree->GetUsedBytes() + V + T + sizeof(*this) + sizeof(*tree);
}

// ============================================================================
// Disk cache of the built model. Raw CRT streaming I/O on purpose: the blob is
// CDB-RAM-sized (GBs on big levels) and IReader would stage a second full copy
// in RAM; fread straight into the destination arrays keeps the load-time peak
// at exactly the model's own footprint.
// ============================================================================
namespace
{
constexpr u32 kCDBCacheMagic = 0x42444358u; // 'XCDB'
constexpr u32 kCDBCacheVersion = 1;

#pragma pack(push, 4)
struct CDBCacheHeader
{
    u32 magic, version;
    u64 key; // caller-provided validity stamp
    u64 verts_count, tris_count;
    u32 nb_nodes, model_code;
};
#pragma pack(pop)

// Node child links are raw pointers into the node array — encoded for disk as
// (index<<1); the low bit stays the OPCODE leaf marker (leaf payloads are kept
// verbatim: they are (primitive<<1)|1, position-independent already).
inline size_t encode_link(size_t v, const AABBNoLeafNode* base)
{
    if (v & 1)
        return v;
    return (size_t)((const AABBNoLeafNode*)v - base) << 1;
}
inline size_t decode_link(size_t v, const AABBNoLeafNode* base)
{
    if (v & 1)
        return v;
    return (size_t)(base + (v >> 1));
}

bool write_all(FILE* f, const void* data, size_t bytes)
{
    const u8* p = (const u8*)data;
    while (bytes)
    {
        const size_t chunk = std::min<size_t>(bytes, 64u << 20);
        if (fwrite(p, 1, chunk, f) != chunk)
            return false;
        p += chunk;
        bytes -= chunk;
    }
    return true;
}

bool read_all(FILE* f, void* data, size_t bytes)
{
    u8* p = (u8*)data;
    while (bytes)
    {
        const size_t chunk = std::min<size_t>(bytes, 64u << 20);
        if (fread(p, 1, chunk, f) != chunk)
            return false;
        p += chunk;
        bytes -= chunk;
    }
    return true;
}
} // namespace

bool MODEL::cache_save(const char* path, u64 key)
{
    static_assert(sizeof(AABBNoLeafNode) == 40, "node layout changed — bump kCDBCacheVersion and re-check encode_link");
    if (S_READY != status || !tree || !tree->GetTree())
        return false;
    // Only the exact shape build() produces (non-quantized no-leaf multi-node tree).
    if (tree->IsQuantized() || tree->HasLeafNodes() || tree->HasSingleNode())
        return false;

    const AABBNoLeafTree* T = (const AABBNoLeafTree*)tree->GetTree();
    const AABBNoLeafNode* nodes = T->GetNodes();
    const u32 nbNodes = T->GetNbNodes();
    if (!nodes || !nbNodes)
        return false;

    string_path tmp;
    xr_sprintf(tmp, "%s.tmp", path);
    FILE* f = fopen(XR_OSPATH(tmp), "wb");
    if (!f)
        return false;

    CDBCacheHeader h{kCDBCacheMagic, kCDBCacheVersion, key, (u64)verts_count, (u64)tris_count, nbNodes, tree->GetModelCode()};
    bool ok = write_all(f, &h, sizeof(h)) && write_all(f, verts, verts_count * sizeof(Fvector)) && write_all(f, tris, tris_count * sizeof(TRI));

    // Nodes: pointer→index fixup streamed through a small scratch block.
    constexpr u32 kBatch = 65536;
    xr_vector<AABBNoLeafNode> scratch;
    scratch.resize(std::min(nbNodes, kBatch));
    for (u32 i = 0; ok && i < nbNodes; i += kBatch)
    {
        const u32 n = std::min(kBatch, nbNodes - i);
        std::memcpy(scratch.data(), nodes + i, n * sizeof(AABBNoLeafNode));
        for (u32 j = 0; j < n; ++j)
        {
            scratch[j].mPosData = encode_link(scratch[j].mPosData, nodes);
            scratch[j].mNegData = encode_link(scratch[j].mNegData, nodes);
        }
        ok = write_all(f, scratch.data(), n * sizeof(AABBNoLeafNode));
    }

    fclose(f);
    if (!ok)
    {
        remove(XR_OSPATH(tmp));
        return false;
    }
    remove(XR_OSPATH(path)); // rename() refuses to overwrite
    if (0 != rename(XR_OSPATH(tmp), XR_OSPATH(path)))
    {
        remove(XR_OSPATH(tmp));
        return false;
    }
    return true;
}

bool MODEL::cache_load(const char* path, u64 key)
{
    R_ASSERT(S_INIT == status);

    FILE* f = fopen(XR_OSPATH(path), "rb");
    if (!f)
        return false;

    Opcode::AABBNoLeafNode* nodes = nullptr;
    Opcode::AABBNoLeafTree* T = nullptr;
    bool ok = false;
    do
    {
        CDBCacheHeader h;
        if (!read_all(f, &h, sizeof(h)))
            break;
        if (h.magic != kCDBCacheMagic || h.version != kCDBCacheVersion || h.key != key)
            break;
        if (!h.verts_count || !h.tris_count || !h.nb_nodes)
            break;
        if (h.model_code & OPC_QUANTIZED || !(h.model_code & OPC_NO_LEAF) || h.model_code & OPC_SINGLE_NODE)
            break;

        verts_count = (size_t)h.verts_count;
        verts = xr_alloc<Fvector>(verts_count);
        tris_count = (size_t)h.tris_count;
        tris = xr_alloc<TRI>(tris_count);
        if (!read_all(f, verts, verts_count * sizeof(Fvector)) || !read_all(f, tris, tris_count * sizeof(TRI)))
            break;

        nodes = new AABBNoLeafNode[h.nb_nodes];
        if (!read_all(f, nodes, (size_t)h.nb_nodes * sizeof(AABBNoLeafNode)))
            break;
        // index→pointer fixup + bounds check (a corrupt cache must fail the load,
        // not stab random memory during the first ray query)
        bool sane = true;
        for (u32 i = 0; i < h.nb_nodes && sane; ++i)
        {
            auto fix = [&](size_t& v) {
                if (v & 1)
                    return; // leaf: (primitive<<1)|1
                if ((v >> 1) >= h.nb_nodes)
                {
                    sane = false;
                    return;
                }
                v = decode_link(v, nodes);
            };
            fix(nodes[i].mPosData);
            fix(nodes[i].mNegData);
        }
        if (!sane)
            break;
        // leaf primitive indices must address our tris
        for (u32 i = 0; i < h.nb_nodes && sane; ++i)
        {
            if ((nodes[i].mPosData & 1) && (nodes[i].mPosData >> 1) >= tris_count)
                sane = false;
            if ((nodes[i].mNegData & 1) && (nodes[i].mNegData >> 1) >= tris_count)
                sane = false;
        }
        if (!sane)
            break;

        T = new AABBNoLeafTree();
        T->SetData(nodes, h.nb_nodes);
        nodes = nullptr; // tree owns them now

        tree = xr_new<Opcode::Model>();
        tree->AdoptTree(T, h.model_code);
        T = nullptr;

        status = S_READY;
        ok = true;
    } while (false);

    fclose(f);
    if (!ok)
    {
        delete[] nodes;
        delete T;
        xr_free(verts);
        verts_count = 0;
        xr_free(tris);
        tris_count = 0;
        status = S_INIT;
    }
    return ok;
}

RESULT& COLLIDER::r_add() { return rd.emplace_back(); }

void COLLIDER::r_dedup_by_id()
{
    if (rd.size() < 2)
        return;
    std::sort(rd.begin(), rd.end(), [](const RESULT& a, const RESULT& b) { return a.id < b.id; });
    rd.erase(std::unique(rd.begin(), rd.end(), [](const RESULT& a, const RESULT& b) { return a.id == b.id; }), rd.end());
}
