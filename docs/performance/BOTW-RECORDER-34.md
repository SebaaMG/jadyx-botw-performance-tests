# BOTW .34: existing parallel recorder, same .30 executable

22 October 2026. Windows 10, Intel Xeon E5-2696 v3, Radeon RX 580, BOTW 1.9.0
with the 60-FPS mod. The owner matched the save and fixed camera and paused HDD
downloads. Each measurement contains four complete windows of 300 frames,
without detailed profiling or manual T capture during the measurement.

## What changed

This is a configuration-only follow-up, not a newly implemented renderer.
The existing `CommandStream` hands ordered command-list operations to
`D3D12Recorder` while the GPU thread prepares subsequent work. It overlaps host
preparation and recording; it does not execute dependent guest draws out of order.
The .29 continuous-submission and .30 synchronization changes remain enabled.

Both runs used the preserved .30 executable, SHA-256:
`ea05f8e531d47d60850c502218be64a4dba4811e6003686629d104430446910c`.
The .34 label identifies packaging/configuration, not a new source build. The
private .31–.33 experiments are absent, rather than merely switched off.

Only `async_record` differs between these paired configurations:

- [Recorder ON](config/boot-pc-recorder-on.cfg): `async_record=1`.
- [Recorder OFF](config/boot-pc-recorder-off.cfg): `async_record=0`.

Both explicitly disable `image_table_reuse`, because the current worker mode
disables that optimization. The original [.30 configuration](config/boot-pc-on.cfg)
keeps image-table reuse enabled and was **not** compared in this pair.
The PC process setting is 8,192 MiB with a 5,120-MiB logical budget in both runs;
this does not describe a change to the Xbox memory allowance.

## Fixed-view result

| Metric | Recorder ON | Recorder OFF |
|---|---:|---:|
| Frames | 1,200 | 1,200 |
| Total ms | 31,083 | 35,428 |
| Mean ms/frame | 25.9025 | 29.5233 |
| FPS | 38.6063 | 33.8715 |
| Draws/frame | 2,829.6575 | 2,830.5658 |
| GPU busy ms/frame | 14.7386 | 15.0747 |
| Resource creation ms/frame | 0.1936 | 0.2848 |
| Pipeline wait ms, total | 0.8 | 0.7 |
| Staging waits, count | 0 | 0 |
| Additional full pool trims, adjacent periodic counters | 0 | 0 |
| P99 per 300-frame window, ms | 39.83 / 38.60 / 38.22 / 37.88 | 40.76 / 44.71 / 41.55 / 42.92 |

Observed improvement: **3.6208 ms/frame less, 12.26% lower mean frame time and
13.98% higher FPS**. Draw count differs by only 0.0321%. Every ON window P99 is
below every OFF window P99; these are window percentiles, **not pooled P99**.
Counter scopes overlap and must not be added as exclusive frame-time components.
No errors were recorded in the selected measurement intervals.

The [aggregate evidence](evidence/botw-34-recorder-on-off-2026-10-22.json)
contains the selected totals and limitations, without personal logs or paths.

## Transition hitches are not solved

After the fixed-view ON capture, the owner reported a slow first scope/zoom,
recovery, and a much faster second opening. A later 300-frame window recorded
P99 278.53 ms, 1,041 pipeline stalls totaling 4,414 ms and 3,285 resource
creations. Sampled committed usage reached 5,507 MiB against the logical
5,120-MiB budget; subsequent windows showed 300 full pool trims and 600 pool
creations per 300 frames. This is logical-budget recovery, not proof of physical
RAM exhaustion or an operating-system OOM.

OFF also had transition hitches: inventory/B and scope/zoom worked, but opening
inventory was slow. A later window recorded P99 329.22 ms, 246.6 ms of pipeline
stalls and 4,519 resource creations. Sampled committed usage reached 5,430 MiB;
the same repeated pool recovery appeared. There are no input timestamps to
attribute either window exclusively to one action. ON inventory was not
independently confirmed. Both sessions exited cleanly through Q with return 0.

Separate T traces were complete: 94,169 events ON and 85,856 OFF, each spanning
120 vsyncs. They were taken after the transition peaks and are not captures of
the first hitch or part of the normal FPS measurement.

## Reproduction and interpretation

Use the published .30 source checkpoint and existing UWP build instructions;
the executable hash identifies the preserved tested artifact, not a guarantee
of byte-identical output from a fresh build. No binary, PGO training data, mod,
save, firmware, keys or decompiled proprietary code is included here.

Apply the paired boot configuration to a local test installation, keep the same
game/mod/save/camera and foreground state, and collect four complete normal
300-frame windows after loading. Restart between configurations. Keep diagnostic
T captures and transition checks separate from the fixed-view timing interval.
Do not use the older capture script's synchronization/supply comparison guards
as a recorder benchmark: those modes intentionally require inline recording.

This was one ON-then-OFF pair, not an interleaved repeat. Persistent caches were
not cleared: the first ON scope visit prepared work before OFF. Startup staging
ring sizes differed (128 MiB ON, 32 MiB OFF), as did memory headroom. Foreground
focus was observed, not continuously monitored. Raw per-frame intervals were
not available in the preserved executable. Therefore the measured fixed-view
benefit is retained provisionally; cold-transition fairness, movement stability,
general game compatibility and a 60-FPS result are not established.

Upstream history, licenses and credits remain unchanged. This publication adds
results and reproduction settings, not another unmeasured source optimization.
