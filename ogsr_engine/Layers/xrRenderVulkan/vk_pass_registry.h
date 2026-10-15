// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// xrRenderVulkan - Minimal render-pass registry (framegraph seam).
//
// The thin seam between "hardcoded pass order in CRender::Render()" and a full
// framegraph. Passes register an ordered Execute callback; ExecutePasses runs
// them in registration order and inserts a SceneAttachmentBarrier between
// adjacent passes (the single-layout convention keeps the color/depth targets
// in their attachment layout the whole frame — see vk_pass_context.h).
//
// To add a pass (G-buffer, SSAO, bloom, lights...) you RegisterPass it instead
// of editing Render(). Targets travel in FrameContext (colorView/depthView), so
// a pass never needs to know it's drawing to the swapchain vs an offscreen RT.
//
// DECLARED DEPENDENCIES (2026-09-03). A pass may now also state, by name, which
// resources it reads and which it writes:
//
//     RegisterPass("World", /*reads*/ { Res::SunCascades, Res::SkinnedPool },
//                           /*writes*/{ Res::SceneColor, Res::SceneDepth },
//                  [](FrameContext& c) { Pass_World(c); });
//
// ValidatePassDeps() then cross-checks the whole list once, on the first frame
// after any registration: a non-history read of a resource whose first declared
// producer runs LATER in the frame is FATAL. That turns "pass A samples what
// pass B produces later" — today a silent one-frame-stale picture nobody
// notices — into a crash on the first run. Reads that genuinely want the
// previous frame wrap in History(res) to say so. Annotation is incremental and
// its failure direction is the safe one; see vk_pass_deps.h for the vocabulary,
// the rules and the checker itself.
//
// BARRIERS FROM THE DECLARATIONS (2026-09-03, r_fg_barriers). The declarations
// now also DRIVE the inter-pass barrier. Instead of one conservative
// SceneAttachmentBarrier — a global memory barrier over colour+depth — between
// every pass pair, each pass Requires only the scene attachments it declared,
// through the same ImageState tracker the depth-read window inside Pass_World
// uses (r_fg_coalesce). The tracker decides whether a barrier is needed, so the
// compute-only head of the frame gets none and the compositing tail's depth
// READS coalesce into one transition. A read declared Sampled(res) is handed
// SHADER_READ instead of the attachment layout and the window is left open for
// the next pass to close, so consecutive samplers share one flip. See the long
// note in vk_pass_registry.cpp for the mechanism and the invariant it depends
// on — that the tracker is the only thing that moves either scene attachment's
// layout inside the pass sequence, which every pass now honours.
// DEFAULT OFF: derivation removes synchronization, and this has not been run on
// hardware yet. r_fg_barriers 2 prints what it costs; one in-level run with
// -vk_validation is what it is waiting for.
//
// Still Execute-only (no Setup phase), and only the two scene attachments are
// derived — the other named resources keep their in-pass barriers. Async-queue
// assignment, the other thing the declarations unlock, is not built.

#pragma once
#include "vk_core.h"
#include "vk_pass_context.h"
#include "vk_pass_deps.h"
#include <functional>
#include <initializer_list>

namespace VK
{
    using PassExecuteFn = std::function<void(FrameContext&)>;

    struct RenderPass
    {
        const char*   name = nullptr;  // debug label
        PassExecuteFn execute;         // records this pass's draws into ctx.cmd
        PassDecl      decl;            // declared reads/writes (decl.declared == false ⇒ not annotated)
        bool          parallel = false; // MarkPassParallel: тело можно писать на воркере (r_mt_record 2)
    };

    // Append a pass to the ordered list (idempotent registration is the caller's
    // job — ExecutePasses runs every registered pass each frame, in order).
    void RegisterPass(const char* name, PassExecuteFn fn);

    // Same, with declared dependencies. `reads` accepts bare Res names (an
    // attachment use), History(Res) for deliberate previous-frame reads, and
    // Sampled(Res) for a read through a descriptor; `writes` is bare names.
    // Declare the reads of what OTHER passes produce (a pass reading back its own
    // output needs no declaration) and ALL the writes other passes may depend on.
    // Over kMaxPassRes entries is a hard error, not a truncation.
    void RegisterPass(const char* name,
                      std::initializer_list<ResUse> reads,
                      std::initializer_list<Res>    writes,
                      PassExecuteFn fn);

    // ⭐РАЗРЕШИТЬ ПАССУ ЗАПИСЫВАТЬСЯ НА ВОРКЕРЕ (`r_mt_record 2`).
    //
    // Согласие ЯВНОЕ и по имени, дефолт -- «нельзя»: безопасное направление тут
    // однозначно. Тело пасса, записываемое на воркере, обязано НЕ трогать
    // порядко-зависимое общее состояние:
    //   * трекер фреймграфа (g_FrameGraph, SceneDepthTo*) -- межпассовый барьер
    //     выводится из деклараций НА ГЛАВНОМ потоке, а Require из тела сдвинет
    //     состояние в чужой момент. Аудит 19-09: из 23 пассов трекер трогает
    //     только World;
    //   * общую RenderQueue (m_GlassItems/m_WaterItems наполняет тело World, а
    //     читают Glass/Water/Particles) -- такие пассы обязаны остаться в порядке;
    //   * зоны Prof:: -- они теперь сами становятся меткой без таблицы на чужом
    //     потоке (vk_profiler.cpp), так что это не запрет, а потеря числа.
    // Кэш конвейеров и пул дескрипторов под блокировкой, вторичные буферы -- по
    // дорожке на поток, так что их использовать можно.
    void MarkPassParallel(const char* name);

    // ⭐ПОДСТАТЬИ ВНУТРИ ТЕЛА ПАССА (та же таблица `[VK RecCPU]`, cpu_budget >= 2).
    //
    // Зачем отдельно от VK_CPU_PROBE: тот мерит ВРЕМЯ СТЕНЫ и печатается под
    // `r_profiler`, а тот сам искажает кадр (19-09: record 1.65 -> 5.23 мс,
    // fps 58 -> 35). Здесь -- ЦИКЛЫ главного потока, ровно как в [VK RecCPU],
    // поэтому подстатьи складываются с целым пассом и сравнимы с cpu_budget.
    //
    // Нужно это для одного вопроса: World -- 58% всей записи (0.52-0.78 core-ms)
    // и один цикл по 639 группам материалов; чтобы его делить, надо знать, какая
    // из внутренних петель несёт цену.
    // ⚠Копит ТОЛЬКО главный поток: накопители -- обычные глобалы, а тело пасса,
    // помеченного MarkPassParallel, идёт на воркере. С чужого потока -- no-op.
    int  RecSubSlot(const char* name);
    void RecSubAdd(int slot, unsigned long long cycles);
    unsigned long long RecSubNow();
    struct RecSubScope
    {
        int slot;
        unsigned long long t0;
        explicit RecSubScope(int s) : slot(s), t0(s >= 0 ? RecSubNow() : 0ull) {}
        ~RecSubScope() { if (slot >= 0) RecSubAdd(slot, RecSubNow() - t0); }
    };

    // ⭐ФАЗА PREPARE: РЕШЕНИЯ КАДРА ДО ЛЮБОЙ ЗАПИСИ (и до раздачи воркерам).
    //
    // Зачем она появилась (19-09-2026, этап Б выноса записи). Оба кандидата на
    // воркер упали НЕ на порядке GPU и не на дескрипторах самих по себе, а на
    // общем ИЗМЕНЯЕМОМ CPU-состоянии, которое одно тело пасса СТРОИТ, пока
    // другое его ЧИТАЕТ:
    //   * `Skinned_UploadBones` (кости + карты поз) -- строит тело PreSkin,
    //     читают SunShadow / World / MV;
    //   * `s_psMap` (лист -> смещение в пуле пре-скиннинга) -- тело PreSkin
    //     строит его ПРЯМО ПО ХОДУ записи своих диспатчей, а читает каждый
    //     рисующий скиннед-пасс;
    //   * набор EnvLight -- обновляет тело World, привязывают Trees/Grass.
    // Пока «решить» и «записать» живут в ОДНОМ теле, любая такая пара -- гонка,
    // независимо от того, который из двух отдан воркеру. Значит, отдельный
    // список пометок тут не поможет: нужен шов.
    //
    // Prepare и есть этот шов: колбэк выполняется на ГЛАВНОМ потоке, в порядке
    // регистрации, ДО первой записанной команды кадра и до раздачи задач. Он
    // НАМЕРЕННО не получает командного буфера: всё, что пишет команды,
    // остаётся в Execute.
    //   RegisterPrepare("PreSkin", [] { VK::Skinned_PreSkinPrepare(); });
    // ⚠Цена при этом не исчезает, а ПЕРЕЕЗЖАЕТ -- она по-прежнему на главном
    // потоке. Выигрыш в том, что запись после этого становится делимой. Прибор
    // [VK RecCPU] печатает фазу отдельной строкой именно поэтому: «подешевело»
    // нельзя путать с «переехало».
    using PassPrepareFn = std::function<void()>;
    void RegisterPrepare(const char* name, PassPrepareFn fn);
    // Run the PREPARE phase NOW, for frame `frameTag`. CRender::Render() calls it as the last act of the EXTRACT side (main
    // thread), because Skin/bones drives game bone callbacks and writes bone_instances / vis.box that the game reads, and the
    // phases fill the frame slot's rings and lists. ExecutePasses does NOT call it (two-slot pipeline: the record side runs on
    // the render thread and must never do extract work). Idempotent per frame slot.
    void RunPrepares(u32 frameTag);

    // Cross-check the declared dependencies of the whole registered list.
    // FATAL on a read-before-write; logs a note for a read with no declared
    // producer (normal while annotation is incomplete). Runs itself from
    // ExecutePasses once per registration change, so nothing has to call it;
    // r_fg_validate 0 downgrades the fatal to a log line.
    void ValidatePassDeps();

    // Print the declared produce/consume table to the log (`vk_pass_deps`).
    void DumpPassDeps();

    // Run every registered pass in order, inserting the inter-pass barrier
    // between adjacent passes: derived from each pass's declared attachment uses
    // (r_fg_barriers 1/2) or the one conservative SceneAttachmentBarrier
    // (r_fg_barriers 0, the default). Passes may early-out internally (no level,
    // etc.) — a pass that declares a write but skips it only costs a barrier
    // nothing needed, which is the safe direction.
    void ExecutePasses(FrameContext& ctx);

    // Drop all registrations (device teardown).
    void ClearPasses();

    // Destroy the GPU pass-timing query pool (device teardown, before vkDestroyDevice).
    void PassTimingDestroy();
}

// ⚠__LINE__ через два уровня -- иначе в имени окажется "__LINE__", а не число.
#define VK_REC_SUB2(name, line) \
    static const int _rsub##line = VK::RecSubSlot(name); \
    VK::RecSubScope _rsubscope##line(_rsub##line)
#define VK_REC_SUB1(name, line) VK_REC_SUB2(name, line)
#define VK_REC_SUB(name) VK_REC_SUB1(name, __LINE__)
