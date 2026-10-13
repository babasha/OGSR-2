# Load-time log readers

Three node scripts that turn a client log into the numbers a load-time change is
judged by. They read `D:\OGSR Gunslinger Addon\_appdata_\logs\xray_egorb_*.log`
and print one row per run, so a series is a single command.

    node _tools/loadprof/total.js <logs...>   # phase table: alife/cform/render x4/spawn/sync + end-to-end
    node _tools/loadprof/play.js  <logs...>   # load cost vs the first 50 gameplay frames (p50/p90/hitches)
    node _tools/loadprof/zones.js <log> [skip]# average GPU zone table over the precache frames

## What "end of load" means here

Not `End of synchronization` — the load screen outlives that marker. With
`keypress_on_start on` (this install) the finished load does not start the level:
it arms an autopause and waits for a key, freezing the global clock, which also
makes the `XROS_CLIENT_CONSOLE` channel go deaf. Both endings are stamped:

    [precache] 28 of 60 frames in 1048 ms (37.4 ms/frame)
    [loadscreen] autopause armed after 1111 ms / 27 frames | 63 ms after the precache ended
    [loadscreen] down after 2024 ms / 49 frames | 874 ms after the precache ended

`play.js` needs gameplay frames, so those runs must dismiss the autopause: set
`XROS_AUTO_SHOT=<dir\prefix>` (it drops the pause after 300 load-screen frames)
and give the harness a long enough `-Hold`.

## Rules that make the numbers mean something

- **Weather decides the frame cost.** An overcast sky puts the sun below the
  luminance threshold, `NightFrozen()` skips the whole VSM update, and a frame
  costs 15 ms instead of 45. Compare interleaved pairs inside one window, never
  run A four times and B four times an hour apart. Rain shows up in the log as
  `[VK Rain] state: density=` / `['dsh_cop_weather']['rain_thun']`.
- **The absolute wall drifts.** Same binary, same level, two hours apart: 5.1 s
  and then 6.7-8.1 s, with every disk-side counter slower in step. Only paired
  comparisons survive that.
- **Deploy only on `BUILD_EXIT_CODE=0`** — a build-and-run one-liner will happily
  copy the previous exe and measure it.

## Runners

    _tools/run_load2.ps1 -Tag t1 -Pre @("r_profiler 2") -Hold 12   # one load, console commands first
    _tools/run_twice.ps1 -Tag rl -Loads 3                          # repeat loads in one process
    _tools/run_series.ps1 -N 5                                     # the repeat-load series

## Knobs the instrumented engine reads

| env / cvar | what it does |
|---|---|
| `XROS_PRECACHE=<n>` | cap the precache frame count |
| `XROS_PRECACHE_ADAPTIVE=0` | keep the fixed count instead of stopping when the warm-up is done |
| `XROS_PRECACHE_SWEEP=0` | hold the spawn view instead of spinning the camera |
| `XROS_PRECACHE_HITCH=1` | dump every precache frame (CPU phases + GPU zones) — feeds `zones.js` |
| `r_vsm_load_freeze 0` | restore the sun-shadow update behind the load screen |
