#pragma once

#include "../../xrCore/xr_resource.h"

class CBackend;
class ECORE_API R_constant_setup;

enum
{
    RC_float = 0,
    RC_int = 1,
    RC_bool = 2,
    RC_sampler = 99,
};
enum
{
    RC_1x1 = 0, // vector1, or scalar
    RC_1x4, // vector4
    RC_1x3, // vector3
    RC_1x2, // vector2
    RC_2x4, // 4x2 matrix, transpose
    RC_3x4, // 4x3 matrix, transpose
    RC_4x4, // 4x4 matrix, transpose
    RC_1x4a, // array: vector4
    RC_3x4a, // array: 4x3 matrix, transpose
    RC_4x4a // array: 4x4 matrix, transpose
};
enum //	Shader stage a constant is bound to
{
    //	Don't change this since some code relies on magic numbers
    RC_dest_pixel = (1 << 0),
    RC_dest_vertex = (1 << 1),
    RC_dest_sampler = (1 << 2),
    RC_dest_geometry = (1 << 3),
    RC_dest_hull = (1 << 4),
    RC_dest_domain = (1 << 5),
    RC_dest_compute = (1 << 6),
};

struct ECORE_API R_constant_load
{
    u16 index; // linear index (pixel)
    u16 cls; // element class

    R_constant_load() : index(u16(-1)), cls(u16(-1)){};

    IC BOOL equal(R_constant_load& C) const { return (index == C.index) && (cls == C.cls); }
};

struct ECORE_API R_constant : public xr_resource
{
    shared_str name; // HLSL-name
    u16 type; // float=0/integer=1/boolean=2
    u32 destination; // pixel/vertex/(or both)/sampler

    R_constant_load ps;
    R_constant_load vs;
    R_constant_load gs;
    R_constant_load hs;
    R_constant_load ds;
    R_constant_load cs;
    R_constant_load samp;
    R_constant_setup* handler;

    R_constant() : type(u16(-1)), destination(0), handler(nullptr){};

    IC R_constant_load& get_load(u32 destination)
    {
        static R_constant_load fake;
        switch (destination & 0xFF)
        {
        case RC_dest_vertex: return vs;
        case RC_dest_pixel: return ps;
        case RC_dest_geometry: return gs;
        case RC_dest_hull: return hs;
        case RC_dest_domain: return ds;
        case RC_dest_compute: return cs;
        default: FATAL("invalid enumeration for shader");
        }
        return fake;
    }

    IC BOOL equal(R_constant& C)
    {
        return !xr_strcmp(name, C.name)
            && type == C.type
            && destination == C.destination
            && ps.equal(C.ps)
            && vs.equal(C.vs)
            && gs.equal(C.gs)
            && hs.equal(C.hs)
            && ds.equal(C.ds)
            && cs.equal(C.cs)
            && samp.equal(C.samp)
            && handler == C.handler;
    }
    IC BOOL equal(R_constant* C) { return equal(*C); }
};
typedef resptr_core<R_constant, resptr_base<R_constant>> ref_constant;

// Automatic constant setup
class ECORE_API R_constant_setup
{
public:
    bool bCapture{};

    virtual void setup(CBackend& cmd_list, R_constant* C) = 0;
    virtual ~R_constant_setup() {}
};

class ECORE_API R_constant_setup_cap : public R_constant_setup
{
public:
    R_constant_setup_cap() { bCapture = true; }
    virtual ~R_constant_setup_cap() {}
};

class ECORE_API R_constant_table : public xr_resource_flagged
{
public:
    typedef xr_vector<ref_constant> c_table;
    c_table table;

private:
    static void fatal(LPCSTR s);

public:
    R_constant_table() = default;
    ~R_constant_table();

    void clear();
    BOOL parse(void* desc, u32 destination);
    void merge(const R_constant_table* T);
    ref_constant get(LPCSTR name) const; // slow search
    ref_constant get(const shared_str& name) const; // fast search
    void dbg_dump(u32 context_id) const;

    BOOL equal(const R_constant_table& C) const;
    BOOL equal(const R_constant_table* C) const { return equal(*C); }
    BOOL empty() const { return 0 == table.size(); }

private:
};
typedef resptr_core<R_constant_table, resptr_base<R_constant_table>> ref_ctable;
