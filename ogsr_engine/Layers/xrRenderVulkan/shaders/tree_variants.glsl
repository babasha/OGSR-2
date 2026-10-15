// xrRenderVulkan — CROWN UBERSHADER VARIANT SPECIALIZATION CONSTANTS.
//
// The same mechanism the world uber-FS uses (world_variants.glsl), for the same
// reason — except on this pass the evidence is unusually direct. Measured
// 26-08-2026 with the per-term probe (r_tree_terms) and the driver's own
// pipeline report (r_pipe_stats):
//
//   * the crown pass costs per WAVE — one surviving lane makes the whole wave
//     execute the whole shader — so a runtime `if` that most lanes skip prices
//     every term at ZERO and hides the cost instead of removing it;
//   * removing a term at PIPELINE-COMPILE time removes its taps, its ALU AND
//     its registers: `r_gather_light 0` (runtime gate) moved Trees/Mesh by 0.02
//     ms, while compiling the same term out moved it by 0.45;
//   * the driver compiles VS and FS as a PAIR and knows which outputs are read.
//     When the FS stops reading a varying, the code that produced it is deleted
//     from the VERTEX shader too (crown VS 56 -> 32 registers, binary 20224 ->
//     4224 bytes, when the gi term goes).
//
// => every gate on this pass that can be known when the pipeline is built MUST
// be a specialization constant. A uniform/cvar branch here is a PICTURE switch,
// never a performance lever. That is the whole reason this file exists.
//
// The mask is carried as ONE int rather than N booleans (unlike the world's
// SPEC_* set) because the crown's probe cvar r_tree_terms hands a mask straight
// through, and because the bit count here is already 12 and still growing. The
// fold is proven either way: mask 0 lands on r_tree_flat 1 to two decimals,
// which is the self-check that the driver really did fold the constant.
//
// ⚠ BIT LAYOUT MUST MATCH TreeSpec IN vk_TreeManager.h. The live mask is built
// by VK::EnvLight::TreeSpecMask() from the SAME UBO fields the branches below
// test — that, and not a comment, is what keeps the two from disagreeing.
#ifndef TREE_VARIANTS_GLSL
#define TREE_VARIANTS_GLSL

#define T_SNOW   0    // SC_SnowAmount frosting            (sf_params.w > 0)
#define T_SUN    1    // sun shadow tap (VSM mask / cascade)(sun colour non-black)
#define T_WET    2    // shoreWet lookup + wet darkening    (rain_params.y > 0 || shorewet live)
#define T_SSIL   3    // screen-space indirect bounce       (r_ssil, IL view bound)
#define T_SKY    4    // SH9 sky ambient                    (always on: costs ~0)
#define T_GI     5    // DDGI probe field in the ambient    (r_ddgi_light) — the expensive one
#define T_AO     6    // GTAO visibility                    (ao_params.z > 0)
#define T_SHEEN  7    // canopy sky specular sheen          (r_ibl)
#define T_DYN    8    // point/spot walk + 1-tap shadows    (dynamic light count > 0)
#define T_FOG    9    // distance fog                       (always on: one mad)
#define T_PTDBG 10    // r_point_debug overlay              (debug only)
#define T_DEBUG 11    // EVERY other debug early-out        (debug only)

// Default = all present, so an unspecialized pipeline — or a twin whose build
// forgets to pass specialization info — behaves EXACTLY like the ubershader did
// before it was split. Silence must fall back to correct, never to fast.
layout(constant_id = 0) const int SPEC_TREE = 0xFFF;

#define TSPEC(bit)             ((SPEC_TREE & (1 << (bit))) != 0)
// TERMQ(bit, expr, off) — `expr` when the term is compiled in, the neutral value
// `off` when it is not. Both sides are written out at the call site so the term
// and its no-op stay next to each other and cannot drift apart.
#define TERMQ(bit, expr, off)  (TSPEC(bit) ? (expr) : (off))
#define SPEC_TDEBUG            TSPEC(T_DEBUG)

#endif // TREE_VARIANTS_GLSL
