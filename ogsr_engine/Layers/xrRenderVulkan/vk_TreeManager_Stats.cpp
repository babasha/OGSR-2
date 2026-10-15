// xrRenderVulkan - Vulkan renderer for X-Ray Engine
// Copyright (c) 2024-2026 Egor Babushkin (https://github.com/babasha)
//
// Original work, "declared otherwise" per the root LICENSE.md. Non-commercial
// use only (per the X-Ray Engine license); redistribution in source or binary
// form must keep this notice and credit the author in-game (credits or splash).

// ============================================================================
//  THE TREE LEDGER  (r_tree_stats, `vk_trees`)
// ============================================================================
//  Trees are the most expensive system in this renderer and, until this file,
//  the least measured one. The frame profiler's `Trees` zone covers ONLY the
//  forward colour pass — the same canopy is ALSO rastered into the camera depth
//  prepass, the motion vectors, the shadow cascades and the VSM atlas, and none
//  of that showed up under a name with "tree" in it. Reading `Trees=5.88` next
//  to `gpu_total=15.5` and concluding "38%" was wrong in the cheap direction:
//  the real share was larger, and spread across zones nobody was looking at.
//
//  Everything attempted on trees so far was steered by an INSTANCE COUNT, and
//  instance counts have pointed the wrong way more than once:
//    • r_tree_hzb shipped on "-18% of instances" with no millisecond ever
//      attached to it. 289 culled saplings and 289 culled canopies are the same
//      number and a 100x different saving.
//    • the cube LOD tier was built, measured and abandoned because the premise
//      "far crowns are many" was true while "far crowns are expensive" was not.
//  So this ledger counts TRIANGLES and FRAGMENTS beside every instance count,
//  and bins the survivors by SCREEN SIZE — the axis a LOD decision is actually
//  made on. If most triangles sit in crowns a few pixels across, a cheaper
//  representation has something to win; if they sit in the 64 px+ bins, no LOD
//  ladder can ever help and the answer is fragment cost. That question now has
//  an answer BEFORE the tier gets built, not after.
//
//  Three layers, all inert at r_tree_stats 0:
//    1. CULL LEDGER — filled by tree_cull.comp (see ST_* there / TreeCullStat
//       in the header). One SSBO, workgroup-aggregated, mirrored to the host.
//    2. HARDWARE COUNTERS — VK_QUERY_TYPE_PIPELINE_STATISTICS around each pass
//       trees are drawn in, plus one PRECISE occlusion query around the colour
//       pass. ⭐ That pair is the point: FS invocations count every fragment
//       shader launch (helper lanes and `discard`ed leaf pixels included),
//       samples-passed counts only the fragments that survived depth AND were
//       not discarded. Their ratio IS the canopy's alpha-test waste, measured
//       instead of assumed — and no atomic in tree.frag was needed to get it.
//    3. CPU PROBES — RenderDepth is a CPU walk over every instance on the
//       level, run once per shadow target per frame. It hides inside CPU_RECORD
//       looking like recording; here it gets counted.
//
//  ⚠ The ledger reads its buffers WITHOUT a fence, i.e. a frame or two stale.
//  That is deliberate: these are steady-state numbers, and stalling the frame to
//  make them current would change the thing being measured.
// ============================================================================

#include "stdafx.h"
#include "vk_render_view.h"
#include "vk_TreeManager.h"
#include "vk_profiler.h"      // VK::Prof::GetZones — the per-pass GPU times the report sums
#include "HW_Vulkan.h"
#include "vk_buffer.h"
#include "vk_command_buffer.h"   // CommandManager.GetCurrentFrame — the query slot ring
#include "CRender_Vulkan.h"
#include "vk_gfx_pipeline.h"     // VK::PipeStats::Dump — the driver's register/occupancy readout

extern int   ps_r_tree_stats;        // r_tree_stats — 0 off, 1 ledger, 2 + per-pass GPU zones
extern float ps_r_tree_dist;
extern float ps_r_tree_vox_dist;
extern float ps_r_tree_shade_dist;
extern int   ps_r_tree_hzb;
extern int   ps_r_tree_zwrite;

namespace VK
{

// The zones that raster or cull trees, and the label each carries in the report.
// ⚠ Names must match the ZoneBegin strings exactly — a typo here shows up as a
// silent 0.00 in the total, which reads like "this pass is free" rather than
// "this pass was not found". The report prints a `missing` note for that reason.
struct TreeZoneRef { const char* zone; const char* label; };
static const TreeZoneRef kTreeZones[] = {
    { "Trees/Cull",   "cull"      },   // the cull compute (r_profiler 2)
    { "Trees/Mesh",   "mesh"      },   // forward colour, crown/trunk meshes
    { "Trees/Vox",    "cubes"     },   // forward crown cube tier (r_tree_vox_dist)
    { "Depth/Trees",  "prepass"   },   // camera depth prepass share
    { "MV/Trees",     "mvec"      },   // wind-sway motion vectors
    { "Sh/TreeDyn0",  "shDyn0"    },   // per-frame near-tree cascade layer
    { "Sh/TreeDyn1",  "shDyn1"    },
    { "Sh/TreeCache0","shCache0"  },   // cached far layer (only on redraw)
    { "Sh/TreeCache1","shCache1"  },
    { "VSM/DynTrees", "vsmDyn"    },   // VSM dynamic caster raster
    { "Bins/TreeS",   "vsmBinS"   },   // VSM binning, static trees
    { "Bins/TreeD",   "vsmBinD"   },   // VSM binning, dynamic trees
    { "Bins/Meshlet", "vsmBinML"  },
    { "Bins/VoxCull", "vsmBinVox" },
};

bool CTreeManager::StatsEnabled()
{
    return ps_r_tree_stats > 0;
}

// ----------------------------------------------------------------------------
// Once per frame, BEFORE any pass records a tree draw and OUTSIDE a render pass
// (vkCmdResetQueryPool is illegal inside one). Harvests the slot recorded
// kFramesInFlight frames ago — its fence has been waited by definition — then
// resets it for this frame.
// ----------------------------------------------------------------------------
void CTreeManager::StatsFrameReset(VkCommandBuffer cmd, u32 frameIndex)
{
    // The CPU counters are per frame and belong to whoever fills them next, so they
    // reset even when the GPU half is unavailable.
    m_CpuDepthWalks = m_CpuDepthSeen = m_CpuDepthDrawn = m_CpuDepthBinds = 0;
    // ⚠ And the prepass cull's validity, for the same reason: it must mean "a dispatch
    // ran THIS frame". Toggling r_tree_depth_gpu off skips the dispatch, and a flag
    // left standing from last frame would have RenderDepth draw stale commands.
    m_DepthCullValid = false;
    // Same for every shadow slot, and here it is not merely hygiene: a cascade's light
    // box MOVES with the camera and the sun. A slot left marked valid would redraw the
    // set that was correct for last frame's box into this frame's map — casters missing
    // on one edge and stale ones on the other.
    m_ShadowCullValid = 0;

    if (!StatsEnabled() || VulkanHW.m_Device == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;
    if (!VulkanHW.m_Device) return;

    if (m_StatPool == VK_NULL_HANDLE)
    {
        VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qci.queryType  = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qci.queryCount = kTreePass * VK_FRAMES_IN_FLIGHT;
        // ⚠ IN BIT ORDER, and TreeStatField mirrors it: vkGetQueryPoolResults writes
        // one u64 per enabled bit ordered by the bit's VALUE, not by the order they
        // are OR'd here. Add a bit without extending TreeStatField and every field
        // after it silently shifts.
        qci.pipelineStatistics =
              VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT
            | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT
            | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(VulkanHW.m_Device, &qci, nullptr, &m_StatPool) != VK_SUCCESS) {
            m_StatPool = VK_NULL_HANDLE;
            Msg("![VK Trees] pipeline-statistics pool create failed — r_tree_stats keeps the cull ledger only");
            return;
        }
        VkQueryPoolCreateInfo oci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        oci.queryType  = VK_QUERY_TYPE_OCCLUSION;
        oci.queryCount = VK_FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(VulkanHW.m_Device, &oci, nullptr, &m_OccPool) != VK_SUCCESS)
            m_OccPool = VK_NULL_HANDLE;   // the rest of the ledger still works
    }

    const u32 slot = frameIndex % VK_FRAMES_IN_FLIGHT;
    if (m_StatUsed[slot])
    {
        // ⚠⚠ ONE SLOT AT A TIME, and that is not fussiness. A query that was reset but
        // never begun — any pass that did not draw a single tree this frame, e.g. the
        // cube tier at its default r_tree_vox_dist 0 — stays UNAVAILABLE forever, and
        // a single unavailable query makes a RANGE read return VK_NOT_READY for the
        // whole range. Reading the range would therefore blank the entire report
        // whenever one optional pass was idle, which is precisely the configuration
        // the report is most often run in. Per-slot, an idle pass simply reads zero
        // (it drew nothing — the honest answer) and its neighbours are unaffected.
        // No WAIT bit anywhere: this slot was recorded VK_FRAMES_IN_FLIGHT frames ago
        // and its fence has been waited by definition.
        for (u32 p = 0; p < kTreePass; ++p) {
            u64 v[kTreeStatFields] = {};
            if (vkGetQueryPoolResults(VulkanHW.m_Device, m_StatPool, slot * kTreePass + p, 1,
                                      sizeof(v), v, sizeof(v), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                memcpy(m_StatVal[p], v, sizeof(v));
            else
                memset(m_StatVal[p], 0, sizeof(v));   // pass idle this frame
        }
        if (m_OccPool != VK_NULL_HANDLE) {
            u64 smp = 0;
            m_OccSamples = (vkGetQueryPoolResults(VulkanHW.m_Device, m_OccPool, slot, 1, sizeof(smp), &smp,
                                                  sizeof(smp), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) ? smp : 0;
        }
        m_StatUsed[slot] = false;
    }

    vkCmdResetQueryPool(cmd, m_StatPool, slot * kTreePass, kTreePass);
    if (m_OccPool != VK_NULL_HANDLE) vkCmdResetQueryPool(cmd, m_OccPool, slot, 1);
    m_StatUsed[slot] = true;
    m_StatOpen = -1;
    m_OccOpen  = false;
}

void CTreeManager::StatsBeginPass(VkCommandBuffer cmd, u32 pass)
{
    if (!StatsEnabled() || m_StatPool == VK_NULL_HANDLE || pass >= kTreePass) return;
    if (m_StatOpen >= 0) return;   // no nesting — see the header note on TP_SHADOW
    const u32 slot = CommandManager.GetCurrentFrame() % VK_FRAMES_IN_FLIGHT;
    vkCmdBeginQuery(cmd, m_StatPool, slot * kTreePass + pass, 0);
    m_StatOpen = (int)pass;
    // The occlusion query brackets the COLOUR pass only: samples-passed is only
    // meaningful where a colour attachment and a real depth test exist, and it is the
    // half of the alpha-waste ratio the statistics pool cannot supply.
    if (pass == TP_COLOR && m_OccPool != VK_NULL_HANDLE && !m_OccOpen) {
        // PRECISE only where the device enabled it — the bit is illegal otherwise,
        // and an imprecise query still answers "did anything survive", just not
        // "how much". See the occlusionQueryPrecise note in HW_Vulkan.cpp.
        vkCmdBeginQuery(cmd, m_OccPool, slot,
                        VulkanHW.m_bOcclusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        m_OccOpen = true;
    }
}

void CTreeManager::StatsEndPass(VkCommandBuffer cmd, u32 pass)
{
    if (!StatsEnabled() || m_StatPool == VK_NULL_HANDLE || pass >= kTreePass) return;
    if (m_StatOpen != (int)pass) return;
    const u32 slot = CommandManager.GetCurrentFrame() % VK_FRAMES_IN_FLIGHT;
    if (pass == TP_COLOR && m_OccOpen) {
        vkCmdEndQuery(cmd, m_OccPool, slot);
        m_OccOpen = false;
    }
    vkCmdEndQuery(cmd, m_StatPool, slot * kTreePass + pass);
    m_StatOpen = -1;
}

void CTreeManager::StatsNoteDepthWalk(u32 seen, u32 drawn, u32 binds)
{
    ++m_CpuDepthWalks;
    m_CpuDepthSeen  += seen;
    m_CpuDepthDrawn += drawn;
    m_CpuDepthBinds += binds;
}

// ----------------------------------------------------------------------------
// The report. Printed every 5 s while r_tree_stats > 0, or immediately on
// `vk_trees`.
// ----------------------------------------------------------------------------
namespace {

// One zone's time, by name. Returns false when the zone does not exist this session
// — a REAL answer (the pass never ran, or r_profiler is below the level that opens
// it), and one the report says out loud rather than folding into the total as zero.
//
// ⚠⚠ `last`, NOT `avg`, IS WHAT THE TOTAL SUMS, and the first run of this report is
// why. ZoneStat::gpuAvg averages the history ring while SKIPPING zero samples
// (vk_profiler.cpp, FoldSample) — so for a zone that only works occasionally, avg is
// the mean of the frames it DID work, not its cost per frame. Two tree zones are
// exactly that: Sh/TreeCache* only redraws when the cached sun map is invalidated,
// and VSM/DynTrees only when the dynamic atlas refreshes. Summing their averages
// billed the canopy for 1.78 + 7.49 ms of work that was not happening in the frame
// being reported, and the total came out at 122% OF THE WHOLE GPU FRAME — a number
// that is its own bug report. Summing `last` puts an idle pass at the 0.00 it
// actually cost. avg still travels alongside, for the noise that one frame carries.
bool ZoneMs(const char* name, float& last, float& avg)
{
    const Prof::ZoneStat* z = nullptr;
    const u32 n = Prof::GetZones(&z);
    for (u32 i = 0; i < n; ++i)
        if (0 == xr_strcmp(z[i].name, name)) { last = z[i].gpuLast; avg = z[i].gpuAvg; return true; }
    return false;
}

// "12.3M" / "845k" / "97" — the ledger deals in numbers spanning six decades and a
// column of raw digits is unreadable at a glance.
const char* Human(u64 v, string32& buf)
{
    if (v >= 100000000ull)   xr_sprintf(buf, "%.0fM", double(v) / 1e6);
    else if (v >= 1000000ull) xr_sprintf(buf, "%.1fM", double(v) / 1e6);
    else if (v >= 10000ull)   xr_sprintf(buf, "%.0fk", double(v) / 1e3);
    else                      xr_sprintf(buf, "%llu", (unsigned long long)v);
    return buf;
}

} // namespace

void CTreeManager::StatsReport()
{
    if (!m_bBuilt) return;

    // ---- 1) THE BILL: every zone that rasters or culls trees, summed. ---------
    // This is the number the `Trees` zone never was. Zones absent from this session
    // are listed rather than silently counted as zero — a missing sub-zone at
    // r_profiler 1 would otherwise read as "that pass costs nothing".
    float total = 0.f, totalAvg = 0.f, gpuTotal = 0.f;
    string1024 bill; bill[0] = 0;
    string512  missing; missing[0] = 0;
    for (const TreeZoneRef& r : kTreeZones)
    {
        float ms = 0.f, avg = 0.f;
        if (ZoneMs(r.zone, ms, avg)) {
            total += ms; totalAvg += avg;
            if (ms >= 0.005f) {
                string64 piece;
                xr_sprintf(piece, "%s%s %.2f", bill[0] ? " | " : "", r.label, ms);
                xr_strcat(bill, piece);
            }
        } else {
            string64 piece;
            xr_sprintf(piece, "%s%s", missing[0] ? "," : "", r.label);
            xr_strcat(missing, piece);
        }
    }
    {   // gpu_total for the share. FrameInfo.gpuMs is the same sum [VK Perf] prints:
        // TOP-LEVEL zones only. Several tree zones are nested inside those (Depth/Trees
        // in World/Depth, Trees/Mesh in Trees), so the tree total is a SLICE of that
        // number, not an addition to it — which is what makes the percentage meaningful
        // and also why no tree zone may ever be nested inside another tree zone.
        const Prof::FrameInfo fi = Prof::GetFrame();
        gpuTotal = fi.gpuMs;
    }
    Msg("[VK TreeCost] trees %.2f ms of %.2f gpu (%.0f%%, avg %.2f) | %s%s%s",
        total, gpuTotal, gpuTotal > 0.01f ? 100.0 * total / gpuTotal : 0.0, totalAvg,
        bill[0] ? bill : "(no tree zones open — raise r_profiler)",
        missing[0] ? " | not open: " : "", missing[0] ? missing : "");

    // ---- 2) THE CULL FUNNEL --------------------------------------------------
    const u32* s = m_CullStatsPtr;
    if (s && s[TS_TESTED])
    {
        // ⚠ Each Human() call gets its OWN buffer. Sharing one would look like it works
        // and print the same number in every column — argument evaluation order is
        // unspecified, so the last writer wins every slot.
        string32 b0, b1, b2, b3, b4, b5;
        Msg("[VK TreeCull] tested %s -> drawn %s | flod %s | frustum %s | hzb %s | cubes %s%s",
            Human(s[TS_TESTED], b0), Human(s[TS_DRAWN], b1), Human(s[TS_CUT_FLOD], b2),
            Human(s[TS_CUT_FRUSTUM], b3), Human(s[TS_CUT_HZB], b4), Human(s[TS_TO_VOX], b5),
            s[TS_OVERFLOW] ? " !!OVERFLOW - trees dropped from the frame" : "");

        // ⭐ The same funnel in TRIANGLES, which is the currency the raster bills in.
        // A knob's worth is its triangle column, not its instance column.
        string32 t0, t1, t2, t3, t4, t5;
        Msg("[VK TreeTris] drawn %s tris (avg %u/tree, heaviest %s) | saved by: flod %s | hzb %s | cubes %s | frustum %s",
            Human(s[TS_TRI_DRAWN], t0),
            s[TS_DRAWN] ? s[TS_TRI_DRAWN] / s[TS_DRAWN] : 0u,
            Human(s[TS_TRI_MAX], t1), Human(s[TS_TRI_FLOD], t2), Human(s[TS_TRI_HZB], t3),
            Human(s[TS_TRI_VOX], t4), Human(s[TS_TRI_FRUS], t5));

        // ---- 3) THE TWO HISTOGRAMS ------------------------------------------
        // Distance is the axis the existing knobs (r_tree_dist, r_tree_vox_dist,
        // r_tree_shade_dist) are expressed in, so it says where a threshold would bite.
        {
            string1024 line; line[0] = 0;
            float lo = kTreeDistBase;
            for (u32 i = 0; i < kTreeHistBins; ++i, lo *= 2.f) {
                if (!s[TS_DIST_N + i]) continue;
                string64 piece; string32 tb;
                xr_sprintf(piece, "%s%.0f-%.0fm:%u/%s", line[0] ? " " : "", i == 0 ? 0.f : lo, lo * 2.f,
                           s[TS_DIST_N + i], Human(s[TS_DIST_TRI + i], tb));
                xr_strcat(line, piece);
            }
            Msg("[VK TreeDist] drawn by distance (trees/tris): %s", line[0] ? line : "-");
        }
        // ⭐⭐ SCREEN SIZE IS THE ONE THAT DECIDES LOD. Two closed arcs (the cube tier,
        // the RT box tier) were built on "there are many far trees" and died on "far
        // trees are not where the cost is". The tail share below is that premise as a
        // number: triangles spent on crowns too small for anyone to resolve.
        {
            string1024 line; line[0] = 0;
            u64 tailTris = 0, allTris = 0;
            float lo = kTreePxBase;
            for (u32 i = 0; i < kTreeHistBins; ++i, lo *= 2.f) {
                allTris += s[TS_PX_TRI + i];
                if (i <= 2) tailTris += s[TS_PX_TRI + i];   // under 8 px across
                if (!s[TS_PX_N + i]) continue;
                string64 piece; string32 tb;
                if (i == kTreeHistBins - 1)
                    xr_sprintf(piece, "%s>%.0fpx:%u/%s", line[0] ? " " : "", lo,
                               s[TS_PX_N + i], Human(s[TS_PX_TRI + i], tb));
                else
                    xr_sprintf(piece, "%s%.0f-%.0fpx:%u/%s", line[0] ? " " : "", lo, lo * 2.f,
                               s[TS_PX_N + i], Human(s[TS_PX_TRI + i], tb));
                xr_strcat(line, piece);
            }
            Msg("[VK TreePx] drawn by screen size (trees/tris): %s | mean %.1f px, largest %u px | UNDER 8px = %.0f%% of tris%s",
                line[0] ? line : "-",
                s[TS_DRAWN] ? double(s[TS_PX_SUM]) / 16.0 / double(s[TS_DRAWN]) : 0.0,
                s[TS_PX_MAX],
                allTris ? 100.0 * double(tailTris) / double(allTris) : 0.0,
                allTris && tailTris * 3 > allTris ? "  -> a cheaper far tier has something to win here"
                                                  : "  => the cost is in BIG crowns; LOD will not help, fragment cost will");
        }
    }
    else if (StatsEnabled())
    {
        Msg("[VK TreeCull] ledger empty — the cull compute has not run a stats frame yet");
    }

    // ---- 4) HARDWARE COUNTERS, per pass -------------------------------------
    if (m_StatPool != VK_NULL_HANDLE)
    {
        static const char* kPassName[kTreePass] = { "prepass", "colour", "cubes", "mvec", "shadow" };
        for (u32 p = 0; p < kTreePass; ++p) {
            const u64* v = m_StatVal[p];
            // ⚠ An idle pass prints "idle" rather than vanishing. A missing line reads
            // as "the instrument is broken"; an explicit zero is information — the VSM
            // dynamic atlas genuinely does not refresh every frame, and the cube tier
            // is off by default. Silence cannot tell those apart from a wiring bug.
            if (!v[TF_IA_PRIMS] && !v[TF_FS_INV]) {
                Msg("[VK TreeHW] %-7s idle this frame (drew nothing)", kPassName[p]);
                continue;
            }
            string32 a, b, c, d;
            Msg("[VK TreeHW] %-7s tris %s -> rasterised %s | vs-inv %s | fs-inv %s",
                kPassName[p], Human(v[TF_IA_PRIMS], a), Human(v[TF_CLIP_PRIMS], b),
                Human(v[TF_VS_INV], c), Human(v[TF_FS_INV], d));
        }
        // ⭐⭐⭐ THE ALPHA-TEST BILL. fs-inv counts every fragment shader launch;
        // samples counts only those that survived depth AND were not discarded. A
        // canopy is layered cutout cards, so the gap between them is leaf alpha being
        // paid for and thrown away — the single number that says whether denser leaf
        // cards (fewer, fuller quads) would pay, and how much.
        const u64 fs = m_StatVal[TP_COLOR][TF_FS_INV];
        if (fs) {
            const double screenPx = double(RV().dwWidth) * double(RV().dwHeight);
            string32 a, b;
            Msg("[VK TreeFill] colour fs-inv %s vs samples-kept %s -> %.0f%% of shaded fragments thrown away (discard+helpers) | canopy overdraw %.2fx of the screen",
                Human(fs, a), Human(m_OccSamples, b),
                fs ? 100.0 * (1.0 - double(m_OccSamples) / double(fs)) : 0.0,
                screenPx > 0.0 ? double(fs) / screenPx : 0.0);
            // ⭐ The whole-frame fragment bill for trees, across every pass. This is the
            // one line to read first on a fill-bound frame: the canopy is alpha-tested
            // cutout cards, so the SAME leaves are shaded again in the prepass, again
            // for motion vectors and again per shadow refresh. Each of those pays the
            // texture fetch and the discard, and only the colour pass produces pixels.
            u64 fsAll = 0;
            for (u32 p = 0; p < kTreePass; ++p) fsAll += m_StatVal[p][TF_FS_INV];
            string32 c;
            Msg("[VK TreeFill] ALL tree passes: fs-inv %s = %.1fx the screen, of which colour %.0f%% / prepass %.0f%% / mvec %.0f%% / shadow %.0f%%",
                Human(fsAll, c), screenPx > 0.0 ? double(fsAll) / screenPx : 0.0,
                fsAll ? 100.0 * double(m_StatVal[TP_COLOR][TF_FS_INV])  / double(fsAll) : 0.0,
                fsAll ? 100.0 * double(m_StatVal[TP_DEPTH][TF_FS_INV])  / double(fsAll) : 0.0,
                fsAll ? 100.0 * double(m_StatVal[TP_MOTION][TF_FS_INV]) / double(fsAll) : 0.0,
                fsAll ? 100.0 * double(m_StatVal[TP_SHADOW][TF_FS_INV]) / double(fsAll) : 0.0);
        }
    }

    // ---- 5) CPU: the per-instance walks nobody was billing -------------------
    // RenderDepth has no GPU cull — it scans m_MetaCPU for EVERY shadow target, every
    // frame. At 193 k instances × a handful of targets that is over a million distance
    // tests hiding inside CPU_RECORD, where it looks like "recording".
    if (m_CpuDepthWalks) {
        string32 a;
        // ⭐ The ratio is the finding, not the total. Scanning 193 471 instances to
        // issue 1 706 draws means 99.1% of that walk exists to say "no" — work the
        // colour path hands to a compute shader and this one still does on the main
        // thread, once per shadow target. The draw count matters too: those are
        // individual vkCmdDrawIndexed calls, not one indirect multi-draw.
        // `binds` is here because it is the cost side of r_tree_depth_sort: drawing in
        // depth slabs re-binds each texture group once per slab it appears in. Both
        // halves of that trade have to be visible in the same line, or the sort looks
        // free right up until the CPU record time says otherwise.
        Msg("[VK TreeCPU] RenderDepth walks %u this frame, %s instances scanned -> %u draws / %u state binds (%.1f%% of the scan was a rejection)",
            m_CpuDepthWalks, Human(m_CpuDepthSeen, a), m_CpuDepthDrawn, m_CpuDepthBinds,
            m_CpuDepthSeen ? 100.0 * (1.0 - double(m_CpuDepthDrawn) / double(m_CpuDepthSeen)) : 0.0);
    }

    // ---- 6) The mode this measurement was taken in ---------------------------
    // ⚠ STAMPED INTO THE MEASUREMENT ON PURPOSE. An A/B compares two logs, and
    // "which run was which" must live in the line, not in the memory of whoever ran
    // it — the same rule the `cut=` stamp on [VK Trees] already follows.
    Msg("[VK TreeMode] r_tree_dist=%.0f vox=%.0f shade=%.0f hzb=%d zwrite=%d | flod-backed %u/%u, vox-backed %u/%u",
        ps_r_tree_dist, ps_r_tree_vox_dist, ps_r_tree_shade_dist, ps_r_tree_hzb, ps_r_tree_zwrite,
        m_FlodBackedCount, m_TotalCount, m_VoxBackedCount, m_TotalCount);
}

// ============================================================================
// vk_pipestats — WHAT THE DRIVER COMPILED THE CROWN INTO
//
// The per-term sweep (r_tree_terms) priced the gi term at 0.45 ms, then the
// no-gather build showed the texture tap inside it is worth nothing — leaving
// "the fragment shader READS vGI.rgb" as the only difference that could carry
// the cost. Five ALU ops and one varying do not cost 0.45 ms linearly; a
// register/occupancy threshold does. Every pipeline below is the SAME shader
// with one thing changed, so their register counts side by side either show a
// step or kill the theory.
//
// Ordered on purpose: shipping first, then the variants in increasing distance
// from it, so the diff reads top to bottom.
// ============================================================================
void CTreeManager::DumpPipelineStats()
{
    Msg("[VK PipeStats] crown pipelines (tcOffset 24 variant of each pair; 28 is the same shader)");
    VK::PipeStats::Dump(m_GfxPipeline24,         "crown SHIPPING");
    VK::PipeStats::Dump(m_GfxPipeline24NoZW,     "crown no-zwrite");
    // The point of the whole exercise: this pair should show the VERTEX shader at
    // the same register count the gi-off term mask produced. If it does not, the
    // per-tree path did not actually take the field lookup out of the VS and the
    // milliseconds have nothing to come from.
    VK::PipeStats::Dump(m_GfxPipeline24GIInst,   "crown PER-TREE GI");
    VK::PipeStats::Dump(m_GfxPipeline24R1,       "crown R1 (nfs31, light per vertex)");
    // The world-state variants — the live crowns. Their register
    // counts next to SHIPPING are the whole question the split exists to answer:
    // a variant that drops terms and keeps the same VGPR count bought nothing,
    // however good the mask looks in the log.
    for (const PermPipes& t : m_PermPipes) {
        string256 off; DescribePermMask(t.mask, off, sizeof(off));
        VK::PipeStats::Dump(t.p24, "crown VARIANT 0x%03X (no %s)", t.mask, off);
    }
    if (m_PermPipes.empty())
        Msg("[VK PipeStats] no crown variants built this run — the world wants every term");
}

} // namespace VK
