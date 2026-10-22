# BOTW performance experiments: source checkpoint .30, configuration follow-up .34

## Latest result: parallel recording .34

The [22 October .34 follow-up](BOTW-RECORDER-34.md) uses the existing .30
executable, not the later rejected renderer experiments. Enabling its existing
parallel recorder reduced mean frame time from **29.5233 to 25.9025 ms** and
increased FPS from **33.8715 to 38.6063** in a 1,200-frame fixed-view comparison.
Paired configurations and aggregate evidence accompany that report. First-use
transition hitches remain; this does not establish stable movement or 60 FPS.

The remainder of this report preserves the earlier .30 source and measurements.

This branch publishes the source and evidence from our Jadyx performance work so
other developers can inspect and build on it. It is an experimental fork of
[JulianDr14/jadyx](https://github.com/JulianDr14/jadyx), retaining the upstream
history, licenses, credits, frontend, and controller flow. No contact with or
endorsement by the upstream developer is implied.

The checkpoint uses upstream commit
`068d86bc21a2807cda95c3699a8792dfc8c2769d` (0.2.93.14). The fork's `main`
publishes this tested checkpoint. The newer upstream commits previously on
`main` remain in Git history; their later changes are not mixed into this source
snapshot.
`.17` through `.30` below are our experiment labels, not upstream release names.
The [full history](HISTORY.md) also preserves the initial .0/.1 controls and every
retained earlier candidate, including experiments later left disabled or dropped.

## Overall progress from original Jadyx

All milestones below were recorded on the same Windows 10 PC with an Intel
Xeon E5-2696 v3 and Radeon RX 580. The owner initially reported roughly 10–15 FPS;
the unmodified upstream capture also contains the numeric baseline:

| Milestone | Recorded scope | FPS | Mean ms/frame |
|---|---|---:|---:|
| Original Jadyx 0.2.93.3 | All 36 complete windows, 10,800 frames, traversal/loading included | 14.78466 | 67.63769 |
| Original, dense-window subset | 24 windows, 7,200 frames, >=3,000 draws/frame after 120 s | 12.52298 | 79.85319 |
| .17 native PGO reference | 1,200 frames, owner-confirmed save/view | 27.45933 | 36.41750 |
| .30, normal 30-FPS cap | 1,200 frames, fixed view | 29.99700 | 33.33667 |
| .30, 60-FPS mod, ON | 1,200 frames, fixed view | 33.66909 | 29.70083 |
| .30, 60-FPS mod, ON repeat | 1,200 frames, fixed view | 33.40292 | 29.93750 |

Against the original all-window capture, the latest observed FPS is
**2.25930–2.27730× (+125.93–127.73%)**, with mean frame time
**37.70019–37.93685 ms lower**. At the normal cap, the recorded progression is
14.78 → 30.00 FPS. This is the historical total, not just the .30 increment.

Sources: [original capture](history/jadyx-windows-2026-10-18.json),
[.17](history/jadyx-pgo-native-2026-10-18.json),
[.30 capped](history/jadyx-firmware-sync-2026-10-20.json), and
[.30 uncapped ON–OFF–ON](evidence/botw-30-on-off-on-2026-10-20.json).
The original session and latest fixed view differ in workload, cache state,
memory settings, upstream revision and FPS mod; the total is an observed
progression, not a controlled original-versus-current speedup. Same-executable
comparisons establish individual increments separately; those percentages
must not simply be added together.

Upstream already reports 30 FPS in lighter Xbox Series X areas and about 20 FPS
in the open world, ahead of the original PC capture. Missing a
33.3-ms deadline can also amplify the visible FPS drop when presentation or
game pacing waits for a later interval; the exact timing chain matters alongside
the amount of work saved. See the original project overview in the root README.

## Earlier measured increment: .30 synchronization

The following comparison isolates the three new .30 options, retaining all
earlier optimizations in both variants. Its gain is additional to the earlier
work, not the original-to-current total.

20 October 2026, Windows 10, Intel Xeon E5-2696 v3 and Radeon RX 580. BOTW 1.9.0,
same 60-FPS mod, owner-confirmed save and fixed camera, downloads paused, same
executable and settings except the three synchronization flags below. Each run
contains four complete windows of 300 presented intervals. Detailed profiling
and manual T capture were inactive during the measurements.

| Run | New synchronization options | Frames | Total ms | Mean ms/frame | FPS |
|---|---|---:|---:|---:|---:|
| ON | All three enabled | 1,200 | 35,641 | 29.70083 | 33.66909 |
| OFF | All three disabled | 1,200 | 37,814 | 31.51167 | 31.73428 |
| ON repeat | All three enabled | 1,200 | 35,925 | 29.93750 | 33.40292 |

Both ON runs were faster than the intervening control: **1.57417–1.81083 ms less
per frame, 5.00–5.75% lower mean frame time, and 5.26–6.10% higher FPS**.
Draw counts were 3,248,245 / 3,246,654 / 3,271,677; upload counts were
733 / 732 / 731. All eight ON window means were below all four OFF window means.
This supports a repeated mean benefit in this scene, not a universal gain or an
individual contribution from each flag.

| Run | P50 per window, ms | P99 per window, ms |
|---|---|---|
| ON | 28.68 / 29.20 / 28.66 / 29.13 | 41.32 / 39.32 / 40.95 / 41.09 |
| OFF | 30.78 / 31.34 / 31.16 / 30.15 | 40.77 / 39.87 / 40.99 / 40.82 |
| ON repeat | 28.82 / 29.06 / 29.11 / 29.17 | 39.12 / 43.11 / 40.36 / 40.46 |

Medians improved; **P99 did not clearly improve**. These are separate window
percentiles, not pooled percentiles. Dynamic scene and host load were not
deterministically replayed. The test is one ON–OFF–ON sequence in one scene.
Earlier 30-FPS-capped tests produced essentially equal means and should not be
used to infer the uncapped gain.

Inventory/B and scope/zoom passed manual checks in the initial ON and OFF runs.
The final ON run has a recorded clean Q shutdown and return 0; its final manual
transition checks were not separately confirmed. No FPS mod, saves, game data,
firmware, keys, shader dumps, binaries, or full personal logs are distributed.

Aggregate values and counter scopes are in
[the numeric evidence](evidence/botw-30-on-off-on-2026-10-20.json).
The `same_exe_sha256` identifies the privately tested executable, not a claim that
an arbitrary fresh build produces identical bytes. Recorded evidence may retain
the earlier pending manual-check field; the final exit observation is separate.

## Changes in this source snapshot

- GPU supply: partial and continuous submissions at safe boundaries, bounded
  outstanding ticks, independent fence-flush coalescing, and query completion
  handling. Required synchronization and resource lifetimes remain enforced.
- Memory: reusable transfer/readback resources, GC policies, compatible copy
  spans, BC1 array upload decoding, and resource-allocation diagnostics.
- Synchronization: nonblocking NV event polling, stable cancellable syncpoint
  callbacks, and BufferQueue consumer fences tied to the actual last GPU use of
  the source image. Out-of-order consumer completion advances only a contiguous
  completion frontier.
- Diagnosis and host optimization: frame-chain tracing, JIT symbol detail,
  opt-in native LTO/PGO support, and separately selectable JIT experiments.

The source also preserves experiments that did not show a useful BOTW gain.
DrawPlan and the unhelpful JIT toggles remain off in the measured configuration.
Adding together historical gains from different runs would be invalid.

The source snapshot before these publication notes and external test harnesses
was tree `60fd40a63ec2b103e0ac89a201de98bd5466ae0d`. It was reconstructed and
checked from 21 source patches (0001–0011 and 0016–0025), excluding the discarded
0012–0015 candidate sequence. Application source in this publication is unchanged
from that snapshot. Historical internal notes can describe an earlier stage;
this report contains the latest published measurements.

## Building and selecting the tested options

Follow the upstream [Xbox/UWP build documentation](../xbox/README.md) for the
Windows SDK, MSVC, Mesa, and signing environment. This is the actual UWP target.
The source retains opt-in defaults; checking out the branch does not enable all
experimental settings automatically. A clean build has not been rerun as part of
publishing this checkpoint; the original incremental build and GPU gates passed.

Native host optimizations are controlled by
`CMakeModules/SeriesXHostOptimization.cmake`. The measured build used native LTO
on `video_core;jadyx` and PGO USE with a locally trained database. That generated
database is not included. For a new build, begin with PGO OFF; if using PGO,
train and compare both variants with the same local database. Enabling USE
without a matching database cannot reproduce the measured build.

The [measured PC boot configuration](config/boot-pc-on.cfg) retains the same
8192-MiB PC process setting and 5120-MiB logical cache budget used in the earlier
comparisons. This is a PC diagnostic configuration, not an increased Xbox memory
allowance. Use the existing 5120-MiB console configuration for console work.

For the control, change only:

```ini
nonblocking_nv_events=0
consumer_release_fences=0
detached_syncpoint_actions=0
```

For ON, set those three values to 1. Restart the emulator between variants;
these options are read at boot. Keep the .29 supply/readback options enabled.
The PC helper in `tools/jadyx/capture-perf-windows.ps1` expects the diagnostic
AppContainer identity `SeriesX.JadyxBinaryProbe_s0hs1mdp2yrec`; an ordinary upstream
registration uses a different identity. Adapt its log path to your registration
before using it. It observes the process and log, and sends no game controls.

The tested EXE, PDB, and signed package were verified locally. They and the private
signing material are not shipped here.

## Tests and analysis tools

The native runner links the production libraries of an already configured UWP
build. It requires Windows PowerShell 5.1, MSVC, the Store CRT, Graphics Tools,
and a runtime layout containing your locally built DLL dependencies:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/jadyx/run-transfer-tests.ps1 -JadyxRoot C:/work/jadyx -RuntimeLayout C:/work/jadyx/build-uwp/package -Mode FirmwareSync
powershell -NoProfile -ExecutionPolicy Bypass -File tools/jadyx/run-transfer-tests.ps1 -JadyxRoot C:/work/jadyx -RuntimeLayout C:/work/jadyx/build-uwp/package -Mode ContinuousSubmit
```

The runner checks inline and asynchronous recording. FirmwareSync uses a small
original test NRO generated by its own source, null-cache fixtures, real core/NV
and BufferQueue code, and a native D3D12 queue. It is not a complete BOTW renderer
or an execution of Nintendo firmware. Previous gates checked pending polls,
cancel/rearm races, nested callbacks, shutdown, wrap, completion order, exact
copied bytes, queries and predication, and clean D3D12 debug output.

The portable trace analyzer fixtures run without a game or GPU:

```powershell
python -m unittest discover -s tests -p test_jadyx_frame_chain.py
python tools/jadyx/analyze_frame_chain.py --log YOUR_LOCAL_TRACE --out YOUR_LOCAL_REPORT
```

T diagnostics are analyzed separately from normal FPS windows. A callback's
inside/outside lock label describes its dispatch route; it does not establish
inherited mutex ownership or compositor identity. `FenceWaitUs` counts a narrower
scheduler wait and does not cover every callback condition-variable wait. Do
not add overlapping elapsed scopes as exclusive CPU time.

## Remaining research

The next concrete target is directed channel waiting (`WAIT_SWITCH`) and
semaphore acquire. The current puller still substitutes a generic fence release
for the requested syncpoint/threshold. A resumable channel scheduler must let an
independent producer progress, preserve partially consumed command state, and
respect flush, invalidation, teardown, and software command completion.

Subsequent candidates include hot macro HLE fallbacks and attributed GPU-to-CPU
downloads. Macro HLE and an x64 macro JIT already exist. Their presence, or a
large number of readbacks, does not establish the remaining critical path.
No performance benefit from these pending changes is claimed.

The firmware comparison produced behavioral contracts; the published changes
are emulator source, with no firmware bytes or decompiler output. Existing GPL
and third-party notices remain in place. See the upstream project licenses and
[acknowledgements](../../README.md#acknowledgements).
