// xrXRC.h: interface for the xrXRC class.
//
//////////////////////////////////////////////////////////////////////

#pragma once

#include "xrCDB.h"

extern XRCDB_API CStatTimer* cdb_clRAY; // total: ray-testing
extern XRCDB_API CStatTimer* cdb_clBOX; // total: box query
extern XRCDB_API CStatTimer* cdb_clFRUSTUM; // total: frustum query

class XRCDB_API xrXRC
{
public:
    CDB::COLLIDER* collider();

public:
    IC void ray_query(u32 options, const CDB::MODEL* m_def, const Fvector& r_start, const Fvector& r_dir, float r_range = 10000.f)
    {
        // A NaN/inf ray defeats every AABB test and walks the ENTIRE tree — ~2s per ray
        // on a 60M-tri level. Broken foreign-map physics objects probe the ground from a
        // NaN position EVERY frame ("walking lag" saga: fps died whenever the actor woke
        // them). Reject degenerate rays outright: no hits, near-zero cost.
        if (!_valid(r_start) || !_valid(r_dir) || !_valid(r_range))
        {
            static u32 s_nan = 0;
            const u32 n = ++s_nan;
            if ((n & (n - 1)) == 0)
                Msg("!![XRC] DEGENERATE ray rejected: start=(%f, %f, %f) dir=(%f, %f, %f) range=%f (hit %u times)", r_start.x, r_start.y, r_start.z, r_dir.x, r_dir.y, r_dir.z,
                    r_range, n);
            collider()->r_clear();
            return;
        }

        cdb_clRAY->Begin();
        // [mod-compat diag] multi-second ray storms on foreign maps: name the slow ones
        // (degenerate dir/range makes OPCODE walk the whole 60M-tri tree per ray)
        static const u64 s_qpf = CPU::QPCFreq();
        const u64 t0 = CPU::QPC();
        collider()->ray_query(options, m_def, r_start, r_dir, r_range);
        const u64 dt = CPU::QPC() - t0;
        if (dt > s_qpf / 20) // > 50ms for ONE ray
        {
            static u32 s_slow = 0;
            const u32 n = ++s_slow;
            if ((n & (n - 1)) == 0)
                Msg("!![XRC] SLOW ray %.0fms: start=(%.2f, %.2f, %.2f) dir=(%.3f, %.3f, %.3f) range=%.1f opt=%x (hit %u times)",
                    double(dt) * 1000.0 / double(s_qpf), r_start.x, r_start.y, r_start.z, r_dir.x, r_dir.y, r_dir.z, r_range, options, n);
        }
        cdb_clRAY->End();
    }

    IC void box_query(u32 options, const CDB::MODEL* m_def, const Fvector& b_center, const Fvector& b_dim)
    {
        cdb_clBOX->Begin();
        collider()->box_query(options, m_def, b_center, b_dim);
        cdb_clBOX->End();
    }

    IC void frustum_query(u32 options, const CDB::MODEL* m_def, const CFrustum& F)
    {
        cdb_clFRUSTUM->Begin();
        collider()->frustum_query(options, m_def, F);
        cdb_clFRUSTUM->End();
    }

    IC CDB::RESULT* r_begin() { return collider()->r_begin(); }
    IC CDB::RESULT* r_end() { return collider()->r_end(); }
    IC size_t r_count() { return collider()->r_count(); }
    IC void r_clear() { collider()->r_clear(); }

    xrXRC();
    ~xrXRC();
};
XRCDB_API extern xrXRC XRC;
