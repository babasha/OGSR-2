// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - DECLARED PASS DEPENDENCIES: the resource vocabulary + the
// pure ordering check that runs over it.
//
// WHY THIS EXISTS. The frame is a list of passes (vk_pass_registry.h) and the
// only thing the registry knew about them was their order. Every "pass A samples
// something pass B produces LATER in the frame" mistake therefore produced a
// picture that is one frame stale — or, on the first frames of a level, garbage —
// and NOTHING said so. That failure never looks like an error: it looks like the
// AO lagging behind the camera, GI that flickers on a cut, a motion field that
// smears where the NPCs are. It gets found by eye, weeks later, if at all.
//
// So passes DECLARE what they read and what they write, by name, and the check
// below turns exactly that class of mistake into a hard failure at startup:
//   a non-history read of a resource whose FIRST declared producer runs LATER in
//   the frame is a violation.
// `history` is the escape hatch for the legitimate case — TAA/SSIL/GI history,
// reprojection sources, anything deliberately sampled one frame late. Marking a
// read as history is a statement that reading last frame's contents is INTENDED.
//
// INCREMENTAL BY CONSTRUCTION. A pass that declares nothing is skipped, as a
// reader AND as a producer, so annotating one pass (or naming one resource) at a
// time never breaks the build. The failure direction is always the safe one:
// under-declaring hides a violation (false negative), it never invents one.
// A read with no declared producer anywhere is a NOTE, not a violation — during
// adoption it usually just means the producer has not been annotated yet.
// ⚠ That safe direction is a property of the ORDERING CHECK only. Once the same
// declarations DERIVE barriers (r_fg_barriers), an under-declaration would remove
// synchronization instead of hiding a finding — so the derivation reads them
// through DeclAccess below, which assumes an unannotated pass touches everything.
//
// GRANULARITY. These are the TOP-LEVEL frame passes. Ordering INSIDE a pass body
// (the prepass -> MV resolve -> SSAO -> gather -> VSM chain inside Pass_World, say)
// is that pass's own business and is invisible here — by design, otherwise every
// pass would have to be split before it could be described.
//
// NO ENGINE DEPENDENCIES ON PURPOSE: plain types only, everything inline. The
// checker is pure (no Vulkan, no globals, no logging) so it can be exercised on
// synthetic pass lists — which is how the table below was verified — while the
// reporting/fatal layer lives in vk_pass_registry.cpp.
//
// This is also the ground the automatic stuff grows from: once reads/writes are
// declared, inter-pass barriers can be DERIVED per resource instead of the one
// conservative SceneAttachmentBarrier ExecutePasses emits today, and async-queue
// assignment has the dependency edges it needs. Neither is built here.

#pragma once

namespace VK
{
    // ---- The named frame resources -----------------------------------------
    // Add entries ONE AT A TIME, as passes get annotated. The only rule: a name
    // must mean the same thing to every pass that uses it (`SceneDepth` is THE
    // scene depth buffer, not "some depth buffer"), because that shared meaning
    // is the whole content of the check.
    enum class Res : unsigned char
    {
        SceneColor,         // HDR scene colour — what every pass composites into
        SceneDepth,         // scene depth/Z (prepass + colour pass + overlays)
        MotionVec,          // screen-space motion vectors (RG16F), for DLSS/FSR/denoise
        SSAO,               // half-res GTAO (+ SSIL) result
        GatherGI,           // final-gather ambient (half-res, traced)
        SunCascades,        // classic sun shadow cascade depth maps
        VolShadowSnapshot,  // downscaled cascade snapshot the async fog inject samples
        VSM,                // virtual shadow map atlas + its screen-space sun mask
        DDGIProbes,         // DDGI irradiance/distance probe volume
        RTScene,            // ray-tracing acceleration structure (TLAS + geometry buffers)
        SkinnedPool,        // compute pre-skinned world-space vertex pool
        SkyIBL,             // prefiltered sky probe (specular mips + SH9)
        SceneColorPrev,     // last frame's composited scene (history by nature)

        COUNT
    };

    inline const char* ResName(Res r)
    {
        switch (r)
        {
        case Res::SceneColor:        return "SceneColor";
        case Res::SceneDepth:        return "SceneDepth";
        case Res::MotionVec:         return "MotionVec";
        case Res::SSAO:              return "SSAO";
        case Res::GatherGI:          return "GatherGI";
        case Res::SunCascades:       return "SunCascades";
        case Res::VolShadowSnapshot: return "VolShadowSnapshot";
        case Res::VSM:               return "VSM";
        case Res::DDGIProbes:        return "DDGIProbes";
        case Res::RTScene:           return "RTScene";
        case Res::SkinnedPool:       return "SkinnedPool";
        case Res::SkyIBL:            return "SkyIBL";
        case Res::SceneColorPrev:    return "SceneColorPrev";
        default:                     return "<unknown>";
        }
    }

    // ---- HOW a pass touches a resource -------------------------------------
    // Only the two scene attachments care today, and only because the derived
    // barrier has to pick a LAYOUT: an attachment use wants COLOR/DEPTH_ATTACHMENT,
    // a sampled use wants SHADER_READ_ONLY. Getting this from the declaration is
    // what lets consecutive samplers SHARE one transition instead of each flipping
    // to sampled and back — see the deferred restore in vk_framegraph.h.
    enum class Use : unsigned char
    {
        Attachment,   // bound as a colour/depth attachment: drawn into, or depth-tested
        Sampled,      // read through a descriptor (a texture fetch, not an attachment)
    };

    // ---- A declared use ----------------------------------------------------
    // Implicitly constructible from Res so the common case reads as a plain list
    // of names: RegisterPass("World", { Res::SunCascades, Res::DDGIProbes }, ...).
    // A bare name means Attachment, which is right for everything that is not one
    // of the two scene attachments (the layout is not derived for those anyway).
    struct ResUse
    {
        Res  res     = Res::COUNT;
        bool history = false;          // true = LAST frame's contents, deliberately
        Use  use     = Use::Attachment;

        ResUse() = default;
        constexpr ResUse(Res r) : res(r) {}
        constexpr ResUse(Res r, bool h) : res(r), history(h) {}
        constexpr ResUse(Res r, bool h, Use u) : res(r), history(h), use(u) {}
    };

    // Wrap a read to say "this samples the PREVIOUS frame's contents, on purpose".
    // Exempts it from the ordering check — so use it when the one-frame lag is the
    // design (temporal history, reprojection), never to silence a real violation.
    constexpr ResUse History(Res r) { return ResUse{ r, true }; }

    // Wrap a read to say "through a descriptor, not as an attachment". For the two
    // scene attachments this is what the derived barrier reads to hand the pass
    // SHADER_READ_ONLY instead of the attachment layout — so declare it only when
    // the pass really never attaches the resource. A pass that samples it AND
    // attaches it (MotionVecDyn) declares both, and gets the attachment layout at
    // the boundary because it can always open its own sample window from there.
    constexpr ResUse Sampled(Res r) { return ResUse{ r, false, Use::Sampled }; }

    // Per-pass declaration capacity. Registration hard-fails on overflow rather
    // than truncating: a silently dropped declaration is a check that lies.
    static constexpr unsigned kMaxPassRes = 12;

    // ---- What the checker sees ---------------------------------------------
    // The declaration half of a RenderPass, split out so the check needs nothing
    // but plain data (no std::function, no Vulkan handles).
    struct PassDecl
    {
        const char*   name       = nullptr;
        bool          declared   = false;   // false ⇒ not annotated yet, skipped entirely
        unsigned char readCount  = 0;
        unsigned char writeCount = 0;
        ResUse        reads [kMaxPassRes]  = {};
        Res           writes[kMaxPassRes]  = {};
    };

    // ---- Reading a declaration ---------------------------------------------
    // What one pass does to one resource. `read`/`write` are what the pass
    // DECLARED, with two rules that exist for the barrier derivation's sake
    // (vk_pass_registry.cpp) rather than for the ordering check:
    //
    //  * an UNDECLARED pass reads AND writes everything. It is the only safe
    //    assumption: a derived barrier REMOVES synchronization, so an unknown
    //    pass has to be assumed to touch whatever is at stake. This is where the
    //    two steps differ — under-declaring merely hid a violation in the
    //    ordering check, but it would be a real race here, so the derivation
    //    treats "not declared" as "assume the worst" and degrades to the
    //    conservative barrier instead of trusting a blank.
    //
    //  * a HISTORY read does not count. It asks for the PREVIOUS frame's
    //    contents, so it must be ordered against last frame's write — which the
    //    frame boundary already does — and ordering it against a write that
    //    comes later in THIS frame would be the opposite of what it wants.
    struct ResAccess
    {
        bool read     = false;   // any non-history read
        bool write    = false;   // declared write (always an attachment write today)
        bool sampled  = false;   // at least one read asked for Sampled
        bool attached = false;   // at least one use needs the attachment layout

        bool touched() const { return read || write; }

        // Which layout the pass has to be handed. A write is an attachment write,
        // and a pass that does both gets the attachment — it can open its own
        // sample window from there, whereas the reverse would leave its rendering
        // scope in the wrong layout.
        bool wantsAttachment() const { return write || attached; }
    };

    inline ResAccess DeclAccess(const PassDecl& d, Res r)
    {
        ResAccess a;
        if (!d.declared) { a.read = a.write = a.attached = true; return a; }
        for (unsigned k = 0; k < d.readCount; ++k)
        {
            if (d.reads[k].res != r || d.reads[k].history) continue;
            a.read = true;
            if (d.reads[k].use == Use::Sampled) a.sampled  = true;
            else                                a.attached = true;
        }
        for (unsigned k = 0; k < d.writeCount; ++k)
            if (d.writes[k] == r) a.write = true;
        return a;
    }

    enum class DepIssue : unsigned char
    {
        ReadBeforeWrite,   // VIOLATION: first declared producer runs later in the frame
        NoProducer,        // note: nothing declares a write of this resource
    };

    struct DepViolation
    {
        DepIssue issue    = DepIssue::NoProducer;
        unsigned reader   = 0;       // index of the reading pass
        unsigned producer = ~0u;     // index of the first declared writer (ReadBeforeWrite only)
        Res      res      = Res::COUNT;
    };

    // Walk `count` passes in execution order and report every declared read that
    // is not satisfied by an earlier declared write. Pure: no logging, no globals.
    //
    // Returns the TOTAL number of issues found; only the first `maxOut` are
    // written to `out` (so a caller can size its buffer and still report a count).
    inline unsigned CheckPassDeps(const PassDecl* passes, unsigned count,
                                  DepViolation* out, unsigned maxOut)
    {
        constexpr unsigned kNone = ~0u;

        // The frame position at which each resource first holds THIS frame's data.
        // FIRST writer, not last: a later write is a legitimate refinement of an
        // already-produced resource (the dynamic motion-vector overlay on top of
        // the static field), and must not make its own consumers look ill-ordered.
        unsigned firstWriter[(unsigned)Res::COUNT];
        for (unsigned r = 0; r < (unsigned)Res::COUNT; ++r) firstWriter[r] = kNone;

        for (unsigned i = 0; i < count; ++i)
        {
            if (!passes[i].declared) continue;
            for (unsigned w = 0; w < passes[i].writeCount; ++w)
            {
                const unsigned r = (unsigned)passes[i].writes[w];
                if (r < (unsigned)Res::COUNT && firstWriter[r] == kNone) firstWriter[r] = i;
            }
        }

        unsigned found = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            if (!passes[i].declared) continue;
            for (unsigned k = 0; k < passes[i].readCount; ++k)
            {
                const ResUse u = passes[i].reads[k];
                if (u.history) continue;                       // last frame's data, declared as such
                const unsigned r = (unsigned)u.res;
                if (r >= (unsigned)Res::COUNT) continue;

                const unsigned fw = firstWriter[r];
                if (fw == kNone)
                {
                    if (found < maxOut) out[found] = { DepIssue::NoProducer, i, kNone, u.res };
                    ++found;
                }
                else if (fw > i)
                {
                    // The read lands BEFORE anything in this frame produced it.
                    if (found < maxOut) out[found] = { DepIssue::ReadBeforeWrite, i, fw, u.res };
                    ++found;
                }
                // fw == i is fine: a pass reading what it wrote itself owns that ordering.
            }
        }
        return found;
    }
}
