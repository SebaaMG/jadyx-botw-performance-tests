# SeriesX .28: bounded partial native submission

Private source candidate on .27, tree 2368d551b423e1014461630ece07343ecb8ad489.
It targets the measured late native delivery: 46 of 51 long SubmitList batches
first reached the D3D12 queue after 98% of their elapsed processing time. The
existing 32 MiB upload-triggered submission remains in place.

## Policy and integration

`partial_submit=1` opts into the policy for new Scheduler instances; zero is
the control. Only completed, actually recorded draw, indirect draw, dispatch,
draw-texture and clear operations count. Skipped/empty operations do not count.
The renderer checks at operation boundaries after local predicates, temporary
views and cache locks have been released. It never cuts inside a draw or a
partially assembled binding set.

The first operation starts the list clock. Thereafter the clock is consulted
every 64 operations, starting at 128 operations. A list may be submitted after
4 ms if fewer than two earlier native ticks remain uncompleted. At most three
latency-triggered submissions are allowed per renderer frame. A natural Flush
resets pending work and its clock but does not replenish that frame budget.
TickFrame replenishes the budget. Upload recovery and required guest/native
fence submissions continue independently of this optional latency budget.

The existing Scheduler::Flush closes query streams, records/submits the list,
advances its native tick, restores counters and invalidates lost command-list
state. Resources/descriptors remain charged to their actual GPU completion
ticks. A partial native tick does not complete a later native tick or the guest
syncpoint: the original fence path still selects and waits for its own tail.
The dedicated operation-boundary scope can propagate a submission exception on
normal exit. It skips optional work while another exception is unwinding and
skips operations already submitted by an upload/guest flush before that exit.
Do not put the new throwing Flush into the existing noexcept SCOPE_EXIT guard.
The policy does not call Finish or wait for the GPU. Background completion is
only queried when a latency check is due. Async recording carries the same
submission through the existing worker in its original order.

Inactive mode returns before updating work counters or reading the clock.
Cumulative partial/clock/backlog statistics are logged every 300 frames. T's
selective trace adds `gpu-partial-submit a=native tick b=completed operations`;
the existing flush/submit origin links then identify the corresponding batch.

## Why bounded submissions

The local Eden Vulkan rasterizer's FlushWork dispatches recording chunks and
periodically calls Flush. D3D12 previously had only its upload-heavy boundary
inside a long batch. Dispatching a CPU recorder chunk alone does not execute
that list on the GPU; the relevant edge is ExecuteCommandLists.

Microsoft documents both native submission ordering and the fixed costs that
batching avoids. AMD likewise recommends minimizing submissions. Consequently
this candidate uses a latency budget, minimum work, per-frame cap and limited
outstanding ticks, rather than sending one list per draw. The constants are an
initial experimental policy; the original trace is not a promise of savings.

Sources:
- [ExecuteCommandLists semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists)
- [AMD RDNA command-buffer and synchronization guidance](https://gpuopen.com/learn/rdna-performance-guide/)
- Local `renderer_vulkan/vk_rasterizer.cpp`, FlushWork; `vk_scheduler.cpp`, Flush/DispatchWork.
- Local `d3d12_rasterizer.cpp`, FlushIfUploadHeavy; `d3d12_scheduler.cpp`, Flush/SubmitRecorded.

## Gates

The pure native policy test covers sampled clocks, minimum work, latency bounds,
clock reversal, ordinary flushes, the frame cap and rearming. GPU tests exercise
the real Scheduler, descriptor queue, shaders and native query slices, with
inline and asynchronous recording. They compare exact output bytes/query sums,
prove a partial tick can complete before the producer is finished, keep later
ticks uncompleted, and hold a real queue fence to exercise bounded backpressure.
Test sleeps create a deterministic latency trigger; these fixtures do not
measure or predict gameplay speed.

Policy test, 48 Python tests, source-sequence reconstruction, UWP Release/LTO/PGO
USE build and both GPU recording modes pass. The GPU gate also checks exception
unwinding and a natural Flush that already submitted the operation before its
boundary scope exits. Pixel output is byte-exact and sliced occlusion-query
totals match the control; D3D12 warnings/errors are absent. These gates prove
correctness and early execution in fixtures. BOTW results follow below.
Use the same executable with only partial_submit switched between one and zero;
keep .26 readback recovery on. For normal FPS measurement, do not press T or
enable detailed draw/JIT profiling. Compare first-native-submit latency, the
native-fence wake chain and normal frame time separately.

## BOTW same-executable comparison, 20 October 2026

The owner loaded the same save/view with downloads paused and the camera still.
Each normal run contains four complete 300-frame windows after confirmation,
without T, sampling or detailed draw/JIT profiling. Only partial_submit changes;
.26 readback recovery stays on. Both runs use EXE SHA-256
aa779c23177ecdf48927fb1f2736dd11b1437d52a6bce5a385874db45c656990.

| Normal measurement | OFF | ON |
|---|---:|---:|
| Frames | 1200 | 1200 |
| Total elapsed ms | 51097 | 46540 |
| Mean ms/frame | 42.5808 | 38.7833 |
| FPS | 23.4847 | 25.7843 |
| Recording-thread fence wait ms/frame | 2.0532 | 0.4424 |
| Software-queue idle ms/frame | 14.5585 | 11.6778 |
| GPU busy counter ms/frame | 15.6218 | 15.6686 |
| Native submissions | 3877 | 7448 |
| Staging creations | 305 | 242 |

Frame time falls 3.7975 ms, or 8.9183%; FPS rises 9.7916% in this comparison.
The p99 values for the four separate 300-interval windows are
63.38/64.30/63.33/63.94 ms OFF and 56.07/55.32/59.31/59.87 ms ON, not pooled p99.
Counter scopes can overlap; do not add their reductions as exclusive savings.
Transfer-pool creations/full trims do not increase within either measured span.
Readback-pressure waits rise by 119 calls/1805.034 ms OFF and zero ON.

After the ON benchmark, the owner captured T: 66453 events, 120 vsyncs,
1987.843 ms, complete/no capacity limit, 152 partial submissions. In 51 batches
over 5 ms, median first-native-submit lag is 4.287 ms and none submits first
after 98% of the batch. The prior .27 diagnostic had 23.119 ms and 46/51 beyond
98%; this is an earlier diagnostic comparison, not the same-executable control.
The owner corrected an initial report: T was not pressed in the OFF run. There
is no OFF diagnostic capture; the already-saved normal benchmark is unaffected.

The results support earlier delivery and improved overlap rather than cheaper
GPU work: GPU busy per frame is similar while waits and wall time decrease.
This is one sequential ON/OFF scene comparison, not a universal speedup or a
30 FPS result. Inventory/B and scope/zoom pass ON. The owner closes both runs
with Q; shutdown, return 0 and process exit are verified. partial_submit=1 is
restored. No controls or timed game shutdown are sent by the agent.

Private verification: verification/partial-submit-v1/comparison.json and
gameplay-on/gameplay-off; source and public-safe evidence live in the owner
SeriesX repository. No personal state, game logs or generated builds go in Git.
