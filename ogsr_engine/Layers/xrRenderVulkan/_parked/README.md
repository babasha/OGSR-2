# `_parked/` — offline SSAO/GTAO analysis tooling

Despite the folder name, nothing here is parked engine code any more. The parked
C++ (an earlier renderer generation: `rvk.cpp`, `vk_lighting.cpp`,
`vk_RenderFactory.cpp`, `vk_shared_stubs.cpp`, `vk_rendertarget.cpp`, …) plus the
`rvk.h` it needed were deleted on 2026-07-29: they referenced both the D3D11
render layer (removed the same day) and `vk_*` headers that never existed, so they
had not been buildable for a long while. Read them out of git history at
`2451bbc32` if something in there is worth porting.

What stays is the Node/Python tooling that diagnosed the SSAO banding saga — see
`docs/claude-memory/vulkan-ssao-gtao.md` for the full story:

| file | what it does |
|---|---|
| `ssao_dump_view.js` | reads `ssao_dump.bin` written while `r_ssao_debug` is on (see `vk_pass_ssao.cpp`); auto-detects 1ch/4ch, emits AO + bent-normal high-pass PNGs, histogram, autocorrelation |
| `gtao_band_repro.js` | offline repro of `ssao.frag` on a synthetic grazing plane (`--filter` / `--bent` / `--image` modes) |
| `gtao_selftest.js` | math self-test of the GLSL port — rerun after any `ssao.frag` change |
| `gtao_selftest.py` | Python twin of the self-test (this machine has no Python; kept for other machines) |
