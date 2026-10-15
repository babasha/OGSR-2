#pragma once

// LoadProf: where one heavy operation on the calling thread (a gameplay spawn)
// spends its time, in named parts that add up to the whole.
//
// Scopes nest, and each keeps only its OWN time: a child's time is taken out of
// its parent. So "model.tex 20 | stalker.net_Spawn 9 | spawn 4" are disjoint
// slices of one spawn, not overlapping totals. Off unless an Arm is alive on
// this thread -- a Scope then costs one thread_local read, so scopes can sit in
// code that also runs at level load or on the main thread.
//
// Header-only on purpose: the engine is linked statically (exe and nro), so the
// inline function's thread_local is one object across all modules.
#include <new>

namespace LoadProf
{
struct Scope;

struct Ctx
{
    // A stalker's spawn alone touches ~35 names (class chain + model + texture
    // + skeleton stages); 32 silently dropped the late ones (the script binder).
    // 96 since the Lua part splits by script function (lua_vm_watch::prof_push).
    // 256 since [frame prof] (07-10): a whole frame_move under one Arm filled 96 with lua:<file>:<line>.
    static constexpr u32 MAX = 256;
    const char* name[MAX];
    u64 clk[MAX];
    u32 calls[MAX];
    u32 n = 0;
    u32 armed = 0;
    Scope* top = nullptr;
    // GOAP planner searches under this Arm ([oh prof]): count, vertices, how many found no plan
    u32 searches = 0, visited = 0, failed = 0;

    void add(const char* nm, u64 c, u32 k = 1)
    {
        for (u32 i = 0; i < n; ++i)
            if (name[i] == nm || 0 == strcmp(name[i], nm))
            {
                clk[i] += c;
                calls[i] += k;
                return;
            }
        if (n == MAX - 1)
            nm = "(table full)";
        if (n == MAX)
        {
            clk[MAX - 1] += c;
            calls[MAX - 1] += k;
            return;
        }
        name[n] = nm;
        clk[n] = c;
        calls[n] = k;
        ++n;
    }
};

inline Ctx& tls()
{
    static thread_local Ctx c;
    return c;
}

struct Scope
{
    Ctx* c;
    const char* nm;
    Scope* parent;
    u64 t0;
    u64 child = 0;

    explicit Scope(const char* name)
    {
        Ctx& x = tls();
        if (!x.armed)
        {
            c = nullptr;
            return;
        }
        c = &x;
        nm = name;
        parent = x.top;
        x.top = this;
        t0 = CPU::QPC();
    }
    ~Scope()
    {
        if (!c)
            return;
        const u64 el = CPU::QPC() - t0;
        c->add(nm, el - child);
        if (parent)
            parent->child += el;
        c->top = parent;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// A part timed by the caller itself (a per-class split of one hot loop, where a
// Scope per iteration would cost more than the iteration): added as k calls and
// taken out of the open Scope, like a child. Only while armed.
inline void Add(const char* name, u64 clk, u32 k)
{
    Ctx& x = tls();
    if (!x.armed)
        return;
    x.add(name, clk, k);
    if (x.top)
        x.top->child += clk;
}

// Arms the table for the outermost operation on this thread; nested Arms
// (a spawn inside a spawn) just ride along and report nothing themselves.
struct Arm
{
    Ctx& c;
    bool outer;

    Arm() : c(tls())
    {
        outer = c.armed++ == 0;
        if (outer)
        {
            c.n = 0;
            c.top = nullptr;
            c.searches = c.visited = c.failed = 0;
        }
    }
    ~Arm() { --c.armed; }

    // Parts of at least min_ms, largest first: "model.tex 20.1 (x3) | spawn 4.0".
    void format(char* buf, size_t sz, double min_ms) const
    {
        buf[0] = 0;
        if (!outer)
            return;
        const double k = 1000.0 / double(CPU::QPCFreq());
        u32 order[Ctx::MAX];
        for (u32 i = 0; i < c.n; ++i)
            order[i] = i;
        std::sort(order, order + c.n, [&](u32 a, u32 b) { return c.clk[a] > c.clk[b]; });
        size_t len = 0;
        for (u32 j = 0; j < c.n && len + 1 < sz; ++j)
        {
            const u32 i = order[j];
            const double ms = k * double(c.clk[i]);
            if (ms < min_ms)
                break;
            const int w = c.calls[i] > 1 ? snprintf(buf + len, sz - len, "%s%s %.1f (x%u)", len ? " | " : "", c.name[i], ms, c.calls[i])
                                          : snprintf(buf + len, sz - len, "%s%s %.1f", len ? " | " : "", c.name[i], ms);
            if (w < 0)
                break;
            len += size_t(w);
        }
    }
};

// A Scope for code that opens and closes in two separate calls and cannot hold
// one on its stack: the Lua VM entry (lua_vm_watch enter/leave, which luabind
// calls without xrCore headers). Only while armed; Pops pair with Pushes.
struct ManualStack
{
    static constexpr u32 MAX = 8;
    alignas(Scope) unsigned char pool[MAX][sizeof(Scope)];
    u32 depth = 0;
    u32 skipped = 0; // Pushes that made no Scope (not armed / full): their Pops just count down
};

inline ManualStack& mstack()
{
    static thread_local ManualStack m;
    return m;
}

inline void Push(const char* name)
{
    ManualStack& m = mstack();
    if (!tls().armed || m.depth >= ManualStack::MAX || m.skipped)
    {
        ++m.skipped;
        return;
    }
    new (m.pool[m.depth++]) Scope(name);
}

inline void Pop()
{
    ManualStack& m = mstack();
    if (m.skipped)
    {
        --m.skipped;
        return;
    }
    if (m.depth)
        reinterpret_cast<Scope*>(m.pool[--m.depth])->~Scope();
}
} // namespace LoadProf
