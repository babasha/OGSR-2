// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// The render-pass registry's implementation. Lived inside CRender_Vulkan.cpp
// while it was three one-liners; moved here when the declared dependencies
// (vk_pass_deps.h) gave it a validator and a dump worth reading on their own.

#include "stdafx.h"
#include "../../xrCore/load_prof.h" // [rt prof]: named parts of a slow render-thread job
#include "vk_pass_registry.h"
#include "../../xrCore/render_thread.h"
#include "vk_barriers.h"        // SceneAttachmentBarrier, ImageState::Require
#include "vk_framegraph.h"      // g_FrameGraph.Color()/Depth() — the tracked scene attachments
#include "vk_profiler.h"        // Prof:: — per-pass GPU/CPU zones
#include "vk_command_buffer.h"  // CommandManager.GetCurrentFrame()
#include "vk_TreeManager.h"     // Trees->StatsFrameReset (r_tree_stats ledger)
#include "vk_pass_secondary.h"  // SecCB:: — запись пасса в свой вторичный буфер
#include "CRender_Vulkan.h"     // RImplementation.Trees
#include "../../xrCore/cpu_budget.h"   // CpuBudget::enabled / CyclesPerMs -- прибор цены записи
#include "../../xrCore/xr_platform_time.h"  // xrPlatform::TickCount64
#include <algorithm>
#include <future>
#ifdef __SWITCH__
void NxGpuMark(VkCommandBuffer cmd, const char* name);   // CRender_Vulkan.cpp: [nx gpu slow] marks
#endif

// r_fg_validate — what a declared-dependency violation costs. 1 (default) = FATAL
// on the first frame, which is the entire point: the mistake it catches is
// invisible in the picture, so it has to stop the run. 0 = log it and keep going,
// the escape hatch for the case where the DECLARATION is what's wrong and someone
// needs the game up to fix it.
extern int ps_r_fg_validate;

// r_fg_barriers — where the inter-pass barrier comes from. 0 (default) = the one
// conservative SceneAttachmentBarrier between every pass pair, exactly as before
// the declarations existed. 1 = DERIVED from them: each pass Requires only the
// scene attachments it actually declared, through the same ImageState tracker
// r_fg_coalesce uses, so a pass touching neither gets no barrier and a run of
// depth READERS coalesces to one transition. 2 = derived + a one-shot line saying
// what it cost, plus the dependency table (audit aid; pair it with -vk_validation).
extern int ps_r_fg_barriers;

// r_mt_record — куда пасс пишет команды. 0 (по умолчанию) = прямо в primary,
// как было всегда. 1 = каждый пасс в СВОЙ вторичный буфер, primary проигрывает
// его тут же через vkCmdExecuteCommands. См. vk_pass_secondary.h: это ЭТАП А,
// механизм без многопоточности.
extern int ps_r_mt_record;

namespace VK
{
// ---------------------------------------------------------------------------
// ⭐ЦЕНА ЗАПИСИ ПАССА В ЦИКЛАХ (прибор для выноса записи на воркеры).
//
// Накопитель по ИНДЕКСУ пасса: список статичен в пределах запуска, а имя нужно
// только для печати. Отчёт раз в 5 с по реальным часам и только при
// cpu_budget >= 2, чтобы прибор не жил в кадре у игрока.
// ⚠Циклы, а не миллисекунды: ожидание фенса и любое вытеснение в цену записи не
// попадают -- см. cpu_budget.h. Перевод в core-ms делается на печати по той же
// калибровке TSC, что у cpu_budget.
// ---------------------------------------------------------------------------
namespace
{
    constexpr u32 kMaxPassStats = 48;

    struct PassCpuStat
    {
        const char* name = nullptr;
        u64         cycles = 0;
    };
    PassCpuStat s_passCpu[kMaxPassStats];
    u32 s_passCpuFrames = 0;
    u32 s_passCpuLastMs = 0;
    // Фаза prepare -- отдельной статьёй, по колбэкам. Без неё перенос CPU-работы
    // из тела пасса в prepare читался бы в отчёте как «пасс подешевел», хотя
    // главный поток платит ровно столько же.
    PassCpuStat s_prepCpu[kMaxPassStats];

    // Подстатьи внутри тел пассов (VK_REC_SUB). Слот интернируется по имени при
    // первом проходе через сайт и дальше живёт в его static -- строковое
    // сравнение делается единицы раз за запуск, а не в кадре.
    constexpr u32 kMaxSubStats = 64;
    PassCpuStat s_subCpu[kMaxSubStats];
    u32         s_subCount = 0;
    u32         s_recMainTid = 0;

    inline u64 PassCycles()
    {
        ULONG64 c = 0;
        QueryThreadCycleTime(GetCurrentThread(), &c);
        return u64(c);
    }

    inline void PassCpuAccum(size_t i, const char* name, u64 cycles)
    {
        if (i >= kMaxPassStats)
            return;
        s_passCpu[i].name = name;
        s_passCpu[i].cycles += cycles;
    }

    void PassCpuMaybeLog(size_t count)
    {
        ++s_passCpuFrames;
        const u32 now = (u32)xrPlatform::TickCount64();
        if (!s_passCpuLastMs) { s_passCpuLastMs = now; return; }
        if (now - s_passCpuLastMs < 5000)
            return;

        const double cpm = CpuBudget::CyclesPerMs();
        const double frames = double(s_passCpuFrames ? s_passCpuFrames : 1);

        // Сортировка по цене: индексы, а не записи -- имя и порядок пасса нужны
        // в выводе, а таблица из 48 элементов сортируется мгновенно.
        u32 order[kMaxPassStats];
        u32 n = 0;
        double total = 0.0;
        for (u32 i = 0; i < kMaxPassStats && i < count; ++i)
        {
            if (!s_passCpu[i].name) continue;
            order[n++] = i;
            total += double(s_passCpu[i].cycles) / cpm;
        }
        std::sort(order, order + n, [](u32 a, u32 b) { return s_passCpu[a].cycles > s_passCpu[b].cycles; });

        Msg("[VK RecCPU] %u passes, %.3f core-ms/frame recorded on the main thread "
            "(over %u frames; mt_record=%d)", n, total / frames, s_passCpuFrames, ps_r_mt_record);
        for (u32 k = 0; k < n; ++k)
        {
            const double ms = double(s_passCpu[order[k]].cycles) / cpm / frames;
            if (ms < 0.002)
                break;
            Msg("[VK RecCPU]   #%02u %-22s %6.3f core-ms/frame  (%4.1f%%)", order[k],
                s_passCpu[order[k]].name, ms, total > 0.0 ? 100.0 * ms * frames / total : 0.0);
        }

        // Подстатьи внутри тел пассов. Печатаются ПЕРЕД prepare, потому что
        // отвечают на тот же вопрос, что таблица выше, только на уровень глубже.
        if (s_subCount)
        {
            u32 sorder[kMaxSubStats];
            u32 sn = 0;
            double stotal = 0.0;
            for (u32 i = 0; i < s_subCount; ++i)
            {
                if (!s_subCpu[i].name) continue;
                sorder[sn++] = i;
                stotal += double(s_subCpu[i].cycles) / cpm;
            }
            std::sort(sorder, sorder + sn, [](u32 a, u32 b) { return s_subCpu[a].cycles > s_subCpu[b].cycles; });
            Msg("[VK RecCPU] sub-stages inside pass bodies: %.3f core-ms/frame accounted", stotal / frames);
            for (u32 k = 0; k < sn; ++k)
            {
                const double ms = double(s_subCpu[sorder[k]].cycles) / cpm / frames;
                if (ms < 0.002) break;
                Msg("[VK RecCPU]     %-24s %6.3f core-ms/frame", s_subCpu[sorder[k]].name, ms);
            }
        }
        // Фаза prepare -- та же таблица, но ДО записи. Печатается всегда, даже
        // нулевой: «здесь ноль» и «строку забыли» должны различаться.
        double prepTotal = 0.0;
        for (u32 i = 0; i < kMaxPassStats; ++i)
            if (s_prepCpu[i].name) prepTotal += double(s_prepCpu[i].cycles) / cpm;
        Msg("[VK RecCPU] prepare phase (main thread, before any recording): %.3f core-ms/frame",
            prepTotal / frames);
        for (u32 i = 0; i < kMaxPassStats; ++i)
        {
            if (!s_prepCpu[i].name) continue;
            const double ms = double(s_prepCpu[i].cycles) / cpm / frames;
            if (ms < 0.002) continue;
            Msg("[VK RecCPU]   pre %-22s %6.3f core-ms/frame", s_prepCpu[i].name, ms);
        }

        for (u32 i = 0; i < kMaxPassStats; ++i)
        {
            s_passCpu[i].cycles = 0;
            s_prepCpu[i].cycles = 0;
        }
        for (u32 i = 0; i < kMaxSubStats; ++i) s_subCpu[i].cycles = 0;
        s_passCpuFrames = 0;
        s_passCpuLastMs = now;
    }
}

    static xr_vector<RenderPass> s_Passes;
    // Cleared by every registration, so a pass registered late (or after a device
    // reset) is checked too, and the check still runs exactly once per list.
    static bool s_DepsValidated = false;

    // Фаза prepare (см. vk_pass_registry.h): решения кадра ДО первой команды.
    // Отдельный список, а не поле пасса: prepare может понадобиться подсистеме,
    // у которой своего пасса в этом списке вообще нет.
    struct PreparePhase { const char* name = nullptr; PassPrepareFn fn; };
    static xr_vector<PreparePhase> s_Prepares;

    void RegisterPass(const char* name, PassExecuteFn fn)
    {
        RenderPass p;
        p.name      = name;
        p.execute   = std::move(fn);
        p.decl.name = name;   // decl.declared stays false → the validator skips it
        s_Passes.push_back(std::move(p));
        s_DepsValidated = false;
    }

    void RegisterPass(const char* name,
                      std::initializer_list<ResUse> reads,
                      std::initializer_list<Res>    writes,
                      PassExecuteFn fn)
    {
        // Truncating would leave the validator checking a subset of what the pass
        // actually does while reporting success — worse than no check at all.
        R_ASSERT3(reads.size()  <= kMaxPassRes, "pass declares too many reads (raise kMaxPassRes)", name);
        R_ASSERT3(writes.size() <= kMaxPassRes, "pass declares too many writes (raise kMaxPassRes)", name);

        RenderPass p;
        p.name          = name;
        p.execute       = std::move(fn);
        p.decl.name     = name;
        p.decl.declared = true;
        for (const ResUse& u : reads)  p.decl.reads [p.decl.readCount++]  = u;
        for (const Res&    w : writes) p.decl.writes[p.decl.writeCount++] = w;
        s_Passes.push_back(std::move(p));
        s_DepsValidated = false;
    }

    void RegisterPrepare(const char* name, PassPrepareFn fn)
    {
        PreparePhase p;
        p.name = name;
        p.fn   = std::move(fn);
        s_Prepares.push_back(std::move(p));
    }

    void ClearPasses() { s_Passes.clear(); s_Prepares.clear(); s_DepsValidated = false; }

    // Two-slot pipeline: the "prepared for frame N" tag is per frame slot, or extract N+1 would stamp it and the record
    // fallback in ExecutePasses would re-run every PREPARE for frame N on the render thread.
    static u32  s_preparedFrame[RT::kSlots] = { u32(-1), u32(-1) };
    static bool s_preparedAny[RT::kSlots]   = { false, false };
    // (body below, next to the CPU stats it feeds)
    void RunPrepares(u32 frameTag)
    {
        if (s_Prepares.empty()) return;                              // not registered yet: ExecutePasses retries
        const u32 ps = RT::Slot();
        if (s_preparedAny[ps] && s_preparedFrame[ps] == frameTag) return;    // already ran this frame
        s_preparedAny[ps] = true; s_preparedFrame[ps] = frameTag;
        const bool prepStats = (CpuBudget::enabled >= 2);
        for (size_t i = 0; i < s_Prepares.size(); ++i)
        {
            if (!s_Prepares[i].fn) continue;
            const u64 a = prepStats ? PassCycles() : 0;
            s_Prepares[i].fn();
            if (prepStats && i < kMaxPassStats)
            {
                s_prepCpu[i].name = s_Prepares[i].name;
                s_prepCpu[i].cycles += PassCycles() - a;
            }
        }
    }

    // --- declared-dependency validation -------------------------------------

    // ------------------------------------------------------------------
    // Подстатьи внутри тела пасса -- см. заголовок. Главный поток фиксируется
    // первым вызовом: тела помеченных пассов идут на воркерах, и их циклы в эту
    // таблицу попадать не должны (накопители общие, да и поток другой).
    // ------------------------------------------------------------------
    unsigned long long RecSubNow()
    {
        return (CpuBudget::enabled >= 2) ? PassCycles() : 0ull;
    }

    int RecSubSlot(const char* name)
    {
        if (!name) return -1;
        for (u32 i = 0; i < s_subCount; ++i)
            if (s_subCpu[i].name && 0 == xr_strcmp(s_subCpu[i].name, name))
                return int(i);
        if (s_subCount >= kMaxSubStats) return -1;
        const int idx = int(s_subCount++);
        s_subCpu[idx].name = name;
        return idx;
    }

    void RecSubAdd(int slot, unsigned long long cycles)
    {
        if (slot < 0 || CpuBudget::enabled < 2) return;
        const u32 tid = GetCurrentThreadId();
        if (!s_recMainTid) s_recMainTid = tid;
        if (tid != s_recMainTid) return;   // чужой поток -- молча мимо
        s_subCpu[slot].cycles += cycles;
    }
    void MarkPassParallel(const char* name)
    {
        for (RenderPass& p : s_Passes)
            if (p.name && 0 == xr_strcmp(p.name, name)) { p.parallel = true; return; }
        // Не FATAL: пасс мог не зарегистрироваться (нет уровня, выключенная
        // подсистема). Но и молчать нельзя -- иначе «почему не ускорилось»
        // ищется в планировщике вместо опечатки в имени.
        Msg("! [VK MT] MarkPassParallel: pass [%s] is not registered", name ? name : "<null>");
    }

    void ValidatePassDeps()
    {
        s_DepsValidated = true;
        if (s_Passes.empty()) return;

        xr_vector<PassDecl> decls;
        decls.reserve(s_Passes.size());
        for (const RenderPass& p : s_Passes) decls.push_back(p.decl);

        u32 declared = 0;
        for (const PassDecl& d : decls) if (d.declared) ++declared;

        // 64 is far more than a real list produces; the count returned is the
        // TOTAL, so an (impossible) overflow still reports honestly.
        DepViolation issues[64];
        const unsigned found = CheckPassDeps(decls.data(), (unsigned)decls.size(), issues, 64);
        const unsigned shown = (found < 64u) ? found : 64u;

        u32 violations = 0, notes = 0;
        for (unsigned i = 0; i < shown; ++i)
        {
            const DepViolation& v  = issues[i];
            const char*         rd = decls[v.reader].name ? decls[v.reader].name : "<unnamed>";
            if (v.issue == DepIssue::ReadBeforeWrite)
            {
                const char* wr = decls[v.producer].name ? decls[v.producer].name : "<unnamed>";
                Msg("![VK FG] pass '%s' (#%u) READS '%s', which is first written by '%s' (#%u) — LATER in the frame. "
                    "Either move the pass after its producer, or mark the read History(Res::%s) if last frame's "
                    "contents are what you actually want.",
                    rd, v.reader, ResName(v.res), wr, v.producer, ResName(v.res));
                ++violations;
            }
            else
            {
                // Expected while the table is incomplete: the producer simply has
                // no declaration yet (or lives outside the registry, like the
                // tonemap composite). Worth saying once, not worth failing on.
                Msg("~[VK FG] pass '%s' (#%u) reads '%s', which no registered pass declares writing "
                    "(producer not annotated yet?)", rd, v.reader, ResName(v.res));
                ++notes;
            }
        }

        Msg("[VK FG] pass deps: %u passes, %u declared, %u violation(s), %u note(s)",
            (u32)decls.size(), declared, violations, notes);

        // With the barriers derived (r_fg_barriers), an unannotated pass is no
        // longer free: DeclAccess has to assume it touches both scene attachments,
        // so its two boundaries fall back to the conservative barrier. Worth saying
        // out loud — it is the difference between "annotate when convenient" and
        // "annotate to get the win".
        const u32 undeclared = (u32)decls.size() - declared;
        if (undeclared && ps_r_fg_barriers)
            Msg("~[VK FG] %u pass(es) are not declared — with r_fg_barriers on, each of "
                "their boundaries falls back to the conservative barrier", undeclared);

        if (violations)
        {
            if (ps_r_fg_validate)
                FATAL("[VK FG] %u declared pass-dependency violation(s) — see the log lines above", violations);
            Msg("![VK FG] r_fg_validate 0 — continuing with %u violation(s); the affected passes read "
                "STALE (previous-frame) data", violations);
        }
    }

    void DumpPassDeps()
    {
        Msg("[VK FG] --- declared pass dependencies (%u passes) ---", (u32)s_Passes.size());
        for (u32 i = 0; i < (u32)s_Passes.size(); ++i)
        {
            const PassDecl& d = s_Passes[i].decl;
            if (!d.declared) { Msg("[VK FG] #%2u %-16s (not declared)", i, s_Passes[i].name); continue; }

            string512 rd; rd[0] = 0;
            for (u32 k = 0; k < d.readCount; ++k)
            {
                if (k) xr_strcat(rd, sizeof(rd), ", ");
                xr_strcat(rd, sizeof(rd), ResName(d.reads[k].res));
                if (d.reads[k].history) xr_strcat(rd, sizeof(rd), "(history)");
            }
            string512 wr; wr[0] = 0;
            for (u32 k = 0; k < d.writeCount; ++k)
            {
                if (k) xr_strcat(wr, sizeof(wr), ", ");
                xr_strcat(wr, sizeof(wr), ResName(d.writes[k]));
            }
            Msg("[VK FG] #%2u %-16s reads [%s]  writes [%s]", i, s_Passes[i].name, rd, wr);
        }
        ValidatePassDeps();   // re-runs the check so the dump ends with its verdict
    }

    // --- barriers derived from the declarations ------------------------------
    //
    // WHAT THIS REPLACES. Between every pass pair ExecutePasses emitted one
    // SceneAttachmentBarrier: a GLOBAL memory barrier over
    // (COLOR_ATTACHMENT_OUTPUT | LATE_FRAGMENT_TESTS, colour+depth writes) ->
    // (COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS, colour+depth read+write).
    // It cost the same whether the pair shared an attachment or not, so the three
    // compute-only passes at the head of the frame (PreSkin, DDGI, SunShadow, none
    // of which touch the scene colour or depth at all) each paid one, and the dozen
    // compositing passes at the tail each re-declared a dependency on depth WRITES
    // that stopped happening back at the LODs pass — pinning every one of their
    // depth tests behind the previous pass's late-fragment stage for nothing.
    //
    // HOW. The declarations say which of the two tracked attachments a pass reads,
    // which it writes, and (for the two whose LAYOUT is derived) whether it wants
    // them attached or sampled. Each pass Requires exactly that — through the SAME
    // ImageState the depth-read window inside Pass_World already routes through
    // (g_FrameGraph, r_fg_coalesce). The tracker, not this code, decides whether a
    // barrier is actually needed: it emits on a real hazard, no-ops a
    // read-after-read at the same layout, and remembers the union of readers so the
    // next write waits on all of them. What falls out:
    //   * a pass declaring neither attachment gets NO barrier;
    //   * the tail's depth READS coalesce to one transition instead of a dozen
    //     dependencies;
    //   * a pass declaring Sampled(SceneDepth) is handed SHADER_READ and the window
    //     is left OPEN, so the water surface and the light cones — adjacent, both
    //     samplers — share one flip where each used to round-trip on its own. The
    //     pass bodies stop touching the layout at all: they declare, the graph
    //     provides. CloseAttachmentWindows restores the frame-wide layout after the
    //     last pass, so a deferred window can never leak downstream;
    //   * and what does get emitted is an IMAGE barrier naming the one image at
    //     stake instead of a global one.
    //
    // THE INVARIANT IT NEEDS. Derivation REMOVES synchronization, so unlike the
    // ordering check it cannot be satisfied by an incomplete declaration:
    //   * an undeclared pass is assumed to read AND write both attachments
    //     (DeclAccess), which degrades that pass's boundaries to the conservative
    //     barrier rather than trusting a blank;
    //   * the tracked layout has to BE the layout. A pass that transitions the scene
    //     colour or depth with a raw ImageBarrier leaves the tracker believing
    //     something false, and a derived barrier would then carry the wrong
    //     oldLayout. This used to rest on a promise — MotionVecDyn, Water and
    //     LightCones each hand-flipped the scene depth to SHADER_READ to sample it,
    //     and Water hand-flipped the scene COLOUR to TRANSFER_SRC for its refraction
    //     blit, all four restoring the attachment layout before returning. They now
    //     go through g_FrameGraph (SceneDepthToSampled/ToAttachment, and Color()
    //     directly for the blit), so inside the pass sequence the tracker is the
    //     only writer of either layout and the invariant is structural rather than
    //     remembered. ⚠ KEEP IT THAT WAY: a new raw ImageBarrier on either scene
    //     attachment between two registered passes silently breaks the derivation.
    //     (Transitions AFTER the sequence — the tonemap composite, DLSS, the
    //     screenshot path — are outside it and harmless: CRender::Begin re-seeds
    //     both from an UNDEFINED transition every frame, so no belief carries over.)
    //
    // NOT DERIVED: the other eleven named resources. Their barriers still live
    // inside the pass bodies — the two scene attachments are the ones the
    // inter-pass barrier was ever about.

    // Last frame's tally, so the A/B is a number rather than an impression.
    struct BarrierStats
    {
        u32 barriers       = 0;  // vkCmdPipelineBarrier2 calls actually issued
        u32 images         = 0;  // image barriers inside them (>= barriers)
        u32 coalesced      = 0;  // Plans the tracker answered with nothing
        u32 untouched      = 0;  // boundaries declaring neither attachment
        u32 freeBoundaries = 0;  // boundaries that declared one but needed no barrier
        u32 closed         = 0;  // deferred windows the sequence closed at the end
    };
    static BarrierStats s_stats;

    // Ask the tracker for the state one pass's declared uses need, and submit
    // whatever it hands back as ONE dependency. Batching matters: six passes write
    // both attachments, so requiring them separately would trade the single global
    // barrier for TWO barrier calls and come out behind on call count even while
    // each call is narrower. Planned together, a boundary costs at most one call —
    // never more than the conservative path, and usually carrying less.
    //
    // Read-only asks for the attachment layout with READ access, so a run of
    // readers coalesces to nothing; a declared write asks for WRITE|READ, because
    // every blending pass in the tail reads the destination it writes.
    static void RequireDeclaredAttachments(VkCommandBuffer cmd, const PassDecl& decl)
    {
        const ResAccess color = DeclAccess(decl, Res::SceneColor);
        const ResAccess depth = DeclAccess(decl, Res::SceneDepth);

        if (!color.touched() && !depth.touched()) { ++s_stats.untouched; return; }

        VkImageMemoryBarrier2 batch[2]{};
        u32 n = 0;

        // Depth first: it is the one that changes layout, so a fixed order keeps the
        // emitted sequence stable frame to frame.
        if (depth.touched())
        {
            ImageState& st = g_FrameGraph.Depth();
            if (st.Valid())
            {
                // A pass that only SAMPLES the depth is handed SHADER_READ, and the
                // window is then left OPEN — the next pass's declaration asks for
                // whatever IT needs, so two consecutive samplers (the water surface,
                // then the light cones) share one flip instead of round-tripping
                // twice each. CloseAttachmentWindows below restores the frame-wide
                // layout after the last pass, so nothing downstream can inherit a
                // sampled depth. The boundary cannot know WHICH stage will sample it,
                // so it covers fragment and compute; a pass wanting a tighter scope
                // opens its own window (SceneDepthToSampled) on top, for free.
                const bool planned = depth.wantsAttachment()
                    ? st.Plan(batch[n], VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        depth.write ? (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
                                    : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)
                    : st.Plan(batch[n], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (planned) ++n; else ++s_stats.coalesced;
            }
        }
        if (color.touched())
        {
            ImageState& st = g_FrameGraph.Color();
            if (st.Valid())
            {
                const bool planned = color.wantsAttachment()
                    ? st.Plan(batch[n], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        color.write ? (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT)
                                    : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT)
                    : st.Plan(batch[n], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (planned) ++n; else ++s_stats.coalesced;
            }
        }

        if (!n) { ++s_stats.freeBoundaries; return; }

        VkDependencyInfo di{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        di.imageMemoryBarrierCount = n;
        di.pImageMemoryBarriers    = batch;
        vkCmdPipelineBarrier2(cmd, &di);
        ++s_stats.barriers;
        s_stats.images += n;
    }

    // Everything downstream of the pass sequence — the tonemap composite, DLSS, the
    // screenshot path — assumes both scene attachments sit in their frame-wide
    // layout, and several of those still hand-place a barrier with that layout as
    // the assumed oldLayout. The deferred restore means the last pass may have left
    // the depth sampled, so the sequence closes its own windows here rather than
    // relying on the last pass happening to want an attachment. Free on a frame
    // where nothing drifted.
    static void CloseAttachmentWindows(VkCommandBuffer cmd)
    {
        ImageState& d = g_FrameGraph.Depth();
        if (d.Valid() && d.layout != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL)
        {
            SceneDepthToAttachment(cmd);
            ++s_stats.barriers; ++s_stats.images; ++s_stats.closed;
        }
        ImageState& c = g_FrameGraph.Color();
        if (c.Valid() && c.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            c.Require(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
            ++s_stats.barriers; ++s_stats.images; ++s_stats.closed;
        }
    }

    // --- frame execution ----------------------------------------------------

    // GPU/CPU pass timing lives in the global profiler (vk_profiler.*): named
    // timestamp zones + CPU time + min/avg/max history + VRAM + debug labels (so
    // Nsight/RenderDoc show our passes by name). ExecutePasses just brackets each
    // pass in a zone.
    #ifdef __SWITCH__
extern "C" { extern int g_xr_nx_bisect; extern int g_xr_nx_slow; extern const char* g_xr_nx_cur_skip; extern int g_xr_nx_ind_tight; extern int g_xr_nx_wskip; }
extern "C" { extern int g_xr_nx_wskip_win; }   // device.cpp nx_statab: the World sub-experiment of the current 5 s window
#endif
void ExecutePasses(FrameContext& ctx)
    {
        // Once per registration change, and deliberately HERE rather than at the
        // end of the registration block: passes registered from anywhere (a
        // subsystem's own Init, a later increment) are then covered by the same
        // check, with no call site to remember.
        if (!s_DepsValidated) ValidatePassDeps();

        LoadProf::Push("rt.profbegin");
        Prof::FrameBegin(ctx.cmd, CommandManager.GetCurrentFrame());
        Prof::MaybeLog();   // emits [VK Perf] every ~5s or on a `vk_perf` MARK
        LoadProf::Pop();

        // Tree ledger (r_tree_stats): harvest the slot from kFramesInFlight frames ago
        // and reset this frame's queries. ⚠ MUST be here — before ANY pass records, and
        // outside every render pass: vkCmdResetQueryPool is illegal inside one, and the
        // first tree draw of the frame belongs to SunShadow, which runs before World.
        if (RImplementation.Trees)
            RImplementation.Trees->StatsFrameReset(ctx.cmd, CommandManager.GetCurrentFrame());

        // ⭐ФАЗА PREPARE -- ДО ВСЕГО ОСТАЛЬНОГО (см. vk_pass_registry.h).
        // Здесь, а не в CRender::Render, по двум причинам: (1) это должно
        // случиться после Prof::FrameBegin, иначе решения кадра окажутся вне
        // кадра профайлера; (2) и обязательно ДО раздачи задач ниже -- вся
        // смысловая нагрузка фазы в том, что к моменту старта первого воркера
        // общее CPU-состояние кадра уже построено и дальше только читается.
        // (The PREPARE phase is NOT run here any more: CRender::Render() runs it on the extract side, on the main thread, for
        // every frame that is recorded. A frame whose extract skipped it -- the ring slot was not proven free -- has no
        // prepared data and draws none of it; running the phase from the render thread would write the rings it skipped.)

        const bool derive = (ps_r_fg_barriers != 0);
        s_stats = BarrierStats{};

        // ⚠ПОРЯДОК ВЫЗОВОВ ТРЕКЕРА ОБЯЗАН СОВПАДАТЬ С ПРЕЖНИМ. Межпассовый
        // барьер выводится из g_FrameGraph, и тела пассов тоже двигают трекер
        // своими Require. Поэтому вторичный буфер НЕ откладывает запись на
        // потом: пасс пишется в свой буфер и тут же проигрывается, барьер
        // по-прежнему стоит между ними. Разнести это по потокам можно только
        // после того, как внутрипассовые барьеры перестанут расставляться
        // руками внутри тел -- см. заметку в vk_framegraph.h.
        const bool useSec = (ps_r_mt_record != 0);
        if (useSec)
            SecCB::BeginFrame(CommandManager.GetCurrentFrame());

        // -------------------------------------------------------------------
        // ⭐ЭТАП Б: ЗАПИСЬ ПОМЕЧЕННЫХ ПАССОВ НА ВОРКЕРАХ (`r_mt_record 2`).
        //
        // Порядок GPU не меняется: primary по-прежнему проигрывает вторичные
        // буферы строго в порядке регистрации, и межпассовый барьер стоит там
        // же, где стоял. Меняется только то, ЧЕМ записан буфер: тело пасса,
        // помеченного MarkPassParallel, уходит в TTAPI и пишется на воркере в
        // СВОЮ дорожку, пока главный поток записывает свои пассы.
        //
        // ⚠Задачи раздаются ДО цикла проигрывания, а не по ходу: иначе главный
        // поток успел бы дойти до пасса раньше, чем воркер начал, и весь
        // выигрыш превратился бы в ожидание.
        // ⚠Дорожка НОМЕР 0 зарезервирована главному потоку -- пул дорожки не
        // потокобезопасен, а главный поток пишет свои пассы параллельно с
        // воркерами.
        // ⚠FrameContext КОПИРУЕТСЯ на задачу (в нём меняется только cmd, а тела
        // пассов ctx не мутируют -- проверено 19-09 grep'ом по всем телам).
        // ⚠Если задачу не удалось поставить или буфер не выдался, пасс честно
        // пишется на главном потоке в порядке -- молчаливой потери пасса быть
        // не должно.
        // -------------------------------------------------------------------
        struct MtJob
        {
            size_t                idx = 0;
            VkCommandBuffer       cb = VK_NULL_HANDLE;
            std::future<void>     done;
        };
        // ⚠МАССИВ ФИКСИРОВАННОГО РАЗМЕРА, А НЕ ВЕКТОР. Задача получает АДРЕС
        // поля cb, чтобы опубликовать в него готовый буфер; вектор при
        // перевыделении этот адрес обнулил бы, и воркер писал бы в память, уже
        // никому не принадлежащую. Дорожек всё равно не больше kMaxLanes.
        static MtJob s_jobs[SecCB::kMaxLanes];
        u32 jobN = 0;

        const bool mt = (ps_r_mt_record >= 2) && (TTAPI != nullptr);
        if (mt)
        {
            u32 lane = 1;   // 0 -- главному потоку
            for (size_t i = 0; i < s_Passes.size() && lane < SecCB::kMaxLanes; ++i)
            {
                if (!s_Passes[i].parallel)
                    continue;
                MtJob& j = s_jobs[jobN++];
                j.idx = i;
                j.cb = VK_NULL_HANDLE;
                const u32 myLane = lane++;
                FrameContext local = ctx;
                RenderPass* pass = &s_Passes[i];
                VkCommandBuffer* outCb = &j.cb;
                // ⚠Лямбда НЕ mutable: TTAPI оборачивает её в std::bind, а тот
                // зовёт callable как const. Поэтому копия контекста делается
                // ВНУТРИ тела, а не правкой захваченной.
                const u32 recSlot = RT::Slot();   // the render thread: the slot of the frame being recorded
                j.done = TTAPI->submit([pass, local, myLane, outCb, recSlot]() {
                    // Pool threads resolve RT::Slot() to the EXTRACT slot; this job records frame N, so it must see frame N's
                    // RV()/lists/snapshots. Pool threads are reused, so restore on the way out.
                    struct SlotScope {
                        int role; u32 slot;
                        SlotScope(u32 s) : role(RT::tl_role), slot(RT::tl_recSlot) { RT::tl_role = RT::Render; RT::tl_recSlot = s; }
                        ~SlotScope() { RT::tl_role = RT::Role(role); RT::tl_recSlot = slot; }
                    } slotScope(recSlot);
                    VkCommandBuffer rec = SecCB::Acquire(myLane);
                    if (rec == VK_NULL_HANDLE)
                        return;                 // не поднялось -- пасс запишет главный поток
                    FrameContext c = local;
                    c.cmd = rec;
                    pass->execute(c);
                    SecCB::Finish(rec);
                    *outCb = rec;               // публикуется через future -- happens-before
                });
                if (jobN >= SecCB::kMaxLanes)
                    break;
            }
        }

        // Найти задачу пасса i (задач единицы, линейный поиск дешевле карты).
        // s_jobs -- статик, его захватывать не нужно и нельзя; jobN -- локальный.
        auto findJob = [&jobN](size_t i) -> MtJob* {
            for (u32 k = 0; k < jobN; ++k)
                if (s_jobs[k].idx == i) return &s_jobs[k];
            return nullptr;
        };

        // ⭐ЦЕНА ЗАПИСИ КАЖДОГО ПАССА В ЦИКЛАХ ГЛАВНОГО ПОТОКА (при cpu_budget >= 2).
        //
        // Зачем ещё один прибор, когда есть зоны Prof:: -- потому что те живут под
        // `r_profiler`, а он сам искажает кадр (19-09: record 1.65 -> 5.23 мс,
        // fps 58 -> 35). Здесь -- QueryThreadCycleTime на границе пасса: два
        // вызова на пасс, тридцать на кадр, и ЦИКЛЫ, то есть ожидание в цену не
        // попадает. Это то число, по которому решается, какие пассы стоит
        // выносить на воркеры и каков потолок выигрыша.
        const bool cpuStats = (CpuBudget::enabled >= 2);
        u64 cycMark = cpuStats ? PassCycles() : 0;

#ifdef __SWITCH__
        // Console-side bisect (-nx_bisect): the engine's GPU timestamps are useless on NVK/GM20B, so find the pass that
        // costs seconds by REMOVING one per frame and reading the wall-clock GPU time of that frame ([nx gpu]).
        // Index == s_Passes.size() means "skip nothing" (the baseline frame of each cycle).
        // Cycle: pass 0..N-1 removed one at a time, N = nothing removed (baseline), N+1 = ALL removed (the fixed cost of a frame:
        // clear + submit + present). No longer gated on a slow frame: the console is at ~60 ms now, the ablation is meaningful.
        int nxSkip = -1;
        bool nxSkipAll = false;
        {
            static int s_cur = 0;
            g_xr_nx_cur_skip = "";
            g_xr_nx_ind_tight = 0;
            // nx_statab.txt (device.cpp): the same World sub-experiments, held for a whole [nx frame] window at the place
            // the player stands -- bisect2 measures the spawn view. 0 = none; bisect2, when on, overrides it per frame.
            g_xr_nx_wskip = g_xr_nx_wskip_win;
            static u32 s_bisectFrames = 0;
            if (g_xr_nx_bisect && ++s_bisectFrames > 3600)
            {
                g_xr_nx_bisect = 0;
                Msg("[nx abl] bisect OFF after %u frames", s_bisectFrames - 1);
            }
            if (g_xr_nx_bisect)
            {
                // ... N+2 = nothing removed, but the IndirectCount stand-in draws exactly last frame's count (no 1.5x+2 headroom).
                const int n = int(s_Passes.size());
                nxSkip = s_cur;
                // ... N+3..N+6 = World sub-parts (g_xr_nx_wskip 1..4), nothing else removed.
                // ... N+7..N+12 = statics colour sub-experiments in the R1 shader (zoff_params.w 1..6, vk_env_light),
                // N+13/N+14 = statics LOD cut: r_cluster_lod x2 (coarser DAG cut), r_cluster_fade 0 (no dithered crossfade band),
                // N+15..N+17 = statics colour with empty commands / 0-instance commands / off-screen vertices (vk_world_gpu DrawColor),
                // N+18..N+21 = terrain: colour without terrain / terrain only / terrain FS constant / terrain without tessellation,
                // N+22 = terrain with the FULL fragment shader instead of the R1 twin (v100),
                // wskip 21..25 = R1 terrain FS without dyn lights / sun shadow / details / lightmap / sky SH (v101).
                // Only the experiments in kActive are cycled (v101: the statics/LOD/command ones are answered).
                static const char* const kWSkip[] = {"(W: no depth prepass)", "(W: no statics colour)", "(W: no rigid dynamics)",
                                                     "(W: no skinned)", "(S: no diffuse tex)", "(S: diffuse mip +1)",
                                                     "(S: no lightmap)", "(S: no dyn lights)", "(S: unlit, tex only)",
                                                     "(S: const colour)", "(L: cluster_lod x2)", "(L: no lod fade)",
                                                     "(S: empty draws)", "(S: 0-inst draws)", "(S: off-screen verts)",
                                                     "(T: no terrain colour)", "(T: terrain only)", "(T: const colour)",
                                                     "(T: no tess)", "(T: full terrain FS)",
                                                     "(TR1: no dyn lights)", "(TR1: no sun shadow)", "(TR1: no details)",
                                                     "(TR1: no lightmap)", "(TR1: no sky SH)", "(TR1: sun 4-tap)",
                                                     "(SH: from SSBO)",
                                                     // 28..30: the nx_statab terrain round (v141) -- labels only here
                                                     "(T: scissor 1x1)", "(T: no tess colour+prepass)", "(T: no z-write)"};
                static const int kActive[] = {1, 2, 4, 16, 18, 20, 21, 22, 23, 24, 25, 27};
                constexpr int nW = int(sizeof(kActive) / sizeof(kActive[0]));
                s_cur = (s_cur + 1) % (n + 3 + nW);
                nxSkipAll = (nxSkip == n + 1);
                const bool tight = (nxSkip == n + 2);
                const int wsub = (nxSkip >= n + 3) ? kActive[nxSkip - (n + 3)] : 0;
                g_xr_nx_ind_tight = tight ? 1 : 0;
                g_xr_nx_wskip = wsub;
                g_xr_nx_cur_skip = wsub ? kWSkip[wsub - 1]
                                 : tight ? "(tight indirect)" : nxSkipAll ? "(ALL passes)" : (nxSkip < n ? s_Passes[nxSkip].name : "(nothing)");
            }
        }
#endif
        for (size_t i = 0; i < s_Passes.size(); ++i)
        {
            // Order pass i's color/depth writes after pass i-1's (no layout change).
#ifdef __SWITCH__
            if (int(i) == nxSkip || nxSkipAll) continue;
#endif

            // First pass needs no barrier: CRender::Begin's TRANSFER_DST→COLOR
            // transition already ordered the clear before any attachment access —
            // and its dst scope is the attachment stages, so it covers every later
            // pass too. Skipping i==0 on BOTH paths keeps the derived path a strict
            // narrowing of the conservative one rather than a different frame start.
            if (i != 0)
            {
                if (derive) RequireDeclaredAttachments(ctx.cmd, s_Passes[i].decl);
                else { SceneAttachmentBarrier(ctx.cmd); ++s_stats.barriers; }
            }
            // Пасс, отданный воркеру: дождаться и проиграть. Ожидание ИМЕННО
            // здесь, в порядке, -- задача пасса i+1 может быть уже готова, но
            // primary принимает буферы только по очереди.
            if (MtJob* job = mt ? findJob(i) : nullptr)
            {
                if (job->done.valid()) job->done.wait();
                if (job->cb != VK_NULL_HANDLE)
                {
                    vkCmdExecuteCommands(ctx.cmd, 1, &job->cb);
#ifdef __SWITCH__
                    NxGpuMark(ctx.cmd, s_Passes[i].name);
#endif
                    if (cpuStats) cycMark = PassCycles();   // чужая работа в цену записи не идёт
                    continue;
                }
                // Воркер не смог -- падаем на обычный путь ниже.
            }

            VkCommandBuffer rec = useSec ? SecCB::Acquire(0) : VK_NULL_HANDLE;
            if (rec == VK_NULL_HANDLE)
            {
                const int z = Prof::ZoneBegin(ctx.cmd, s_Passes[i].name);
                { LoadProf::Scope _lp(s_Passes[i].name); s_Passes[i].execute(ctx); } // [rt prof]: the pass by name
                Prof::ZoneEnd(ctx.cmd, z);
            }
            else
            {
                // Зона остаётся ВОКРУГ ТЕЛА, а не вокруг ExecuteCommands: иначе
                // CPU-число перестало бы быть ценой записи этого пасса, а именно
                // её мы и пытаемся распараллелить.
                VkCommandBuffer saved = ctx.cmd;
                ctx.cmd = rec;
                const int z = Prof::ZoneBegin(rec, s_Passes[i].name);
                { LoadProf::Scope _lp(s_Passes[i].name); s_Passes[i].execute(ctx); } // [rt prof]: the pass by name
                Prof::ZoneEnd(rec, z);
                ctx.cmd = saved;
                SecCB::Finish(rec);
                vkCmdExecuteCommands(ctx.cmd, 1, &rec);
            }
#ifdef __SWITCH__
            NxGpuMark(ctx.cmd, s_Passes[i].name);   // [nx gpu slow]: GPU time up to the end of this pass
#endif

            if (cpuStats)
            {
                const u64 now = PassCycles();
                PassCpuAccum(i, s_Passes[i].name, now - cycMark);
                cycMark = now;
            }
        }
        if (cpuStats)
            PassCpuMaybeLog(s_Passes.size());
        // Deferred sample windows end here (no-op unless one is actually open).
        if (derive) CloseAttachmentWindows(ctx.cmd);

        // r_fg_barriers 2 — one-shot audit of what the derivation actually cost, so
        // the A/B is a number and not an impression. Printed once per toggle rather
        // than per frame: the declarations are static, so the decisions are identical
        // every frame. Pair it with -vk_validation, which is what would catch a pass
        // breaking the tracked-layout invariant documented above.
        if (ps_r_fg_barriers >= 2)
        {
            static int s_loggedFor = -1;
            if (s_loggedFor != ps_r_fg_barriers)
            {
                s_loggedFor = ps_r_fg_barriers;
                const u32 pairs = (u32)(s_Passes.size() ? s_Passes.size() - 1 : 0);
                Msg("[VK FG] derived barriers: %u barrier call(s) carrying %u image "
                    "barrier(s), vs %u global memory barrier(s) on the conservative path "
                    "(r_fg_barriers 0). Of %u pass boundaries, %u declared neither scene "
                    "attachment and %u needed no barrier once the tracker was asked; "
                    "%u attachment require(s) coalesced away, %u deferred window(s) "
                    "closed at the end.",
                    s_stats.barriers, s_stats.images, pairs, pairs,
                    s_stats.untouched, s_stats.freeBoundaries, s_stats.coalesced, s_stats.closed);
                DumpPassDeps();
            }
        }

        // NOTE: FrameEnd is NOT called here — the Tonemap composite runs after
        // ExecutePasses (CRender::Render) and is wrapped in its own zone there,
        // so FrameEnd closes the frame after Tonemap. (Tonemap hosts the SSR
        // puddle march — a key rain cost — so it must be profiled too.)
    }

    void PassTimingDestroy() { Prof::Shutdown(); SecCB::Destroy(); }
}
