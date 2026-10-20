# SeriesX .27: selective frame-chain diagnostic

Private diagnostic on the .26 readback-recovery source tree. It does not change
emulation, scheduling policy, swap intervals, buffer ownership or fences. The
.17 and .26 executables and symbols remain private performance references.

## Activation and cost

`frame_chain_trace=1` selects this event policy; `developer_hotkeys=1` enables
the existing manual T capture. Neither setting starts a capture. T records at
most 120 vsyncs in the existing fixed 524288-entry storage, then dumps the log.
The dump can stall presentation; this is not an FPS benchmark. No per-draw
profile, guest PC/context extraction, memory callbacks or pipeline detail is
enabled by this policy. Setting the mode to zero retains the legacy focused
guest filter. The mode cannot change while recording.

Guest scheduling, SVC and original IPC-request IDs are unrestricted during this
capture. Existing host CPU windows are sampled roughly every 100 ms, not at
every guest Run. Their OS CPU totals are distinct from Run/SVC elapsed spans.
Elapsed spans include callbacks and host preemption and are not CPU utilization.
Outside a capture the new scopes do not read clocks or allocate event storage.

## What is connected

The actual route in this source is service IPC -> nvhost submit -> software GPU
queue -> `Control::Scheduler::Push` -> Maxwell DMA interpretation -> D3D12
recording -> native submission. Maxwell processing happens AFTER PopWait, not
on a parallel producer before this queue. A software queue wait measures missing
queued work; it does not identify what kept the original requester from sending.

IPC scopes use `ctx.GetThread().GetThreadId()`, not the current HLE worker ID.
The offline analyzer associates enqueue/dequeue with an enclosing original
request on the same host thread. A request migrating host threads is not silently
attributed. There is no thread-local IPC owner that could leak between fibers.

The dedicated native GPU worker scopes a capture-tagged batch origin. Flush
carries that origin through the D3D12 recorder lambda, including asynchronous
recording. Missing/cross-capture origin is zero, never a guessed guest ID.
The marker describes the batch that triggered a Flush; the native list may
contain commands accumulated by earlier batches and is not exclusively owned
by that one guest batch. Native submit is not GPU completion.

Framebuffer records use stable queue identity plus guest frame number and slot,
not frame number alone. Dequeue wait, actual returned/acquire fences, queue,
acquire, release decision, actual release and dequeue notification are recorded.
The existing release-fence TODO and frame-lease policy are intentionally intact;
the diagnostic must not make an unsafe early release look like an optimization.

## Payload schema

All rows are `FT milliseconds host-thread event a b`; the header identifies
`frame-chain-v1`, capture ID and requested vsyncs.

| Events | a | b |
| --- | --- | --- |
| ipc-request-begin/end | request identity | original guest ID |
| buffer-dequeue-begin/end | request identity | buffer queue identity |
| gpu-queue-push-begin/end, gpu-work-begin/end | software batch fence | kind high32, channel low32 |
| gpu-queue-wait | PopWait elapsed us | next batch fence |
| gpu-queue-lock-long | write-lock wait us | kind/channel |
| gpu-flush-recorded, gpu-submit | native scheduler tick | triggering software batch, zero unknown |
| buffer-queued/acquired/released-frame | queue identity | guest frame number |
| buffer-queued/acquired/released-slot | queue identity | slot low32; queued swap interval high32 |
| buffer-dequeued | queue identity | returned slot |
| buffer-wait-begin/end | queue identity | packed slot state / waited us |
| buffer-returned/queued-fence | queue identity | syncpoint low32, target high32 |
| frame-lease-queue | consumer ID | queue identity |
| buffer-dequeue-signal | queue identity | zero |

Kind 1 is SubmitList; kind 5 is GpuTick. Non-channel commands use channel
0xffffffff. Slot metadata pairs with the preceding same-host/queue/stage frame
record; do not assume globally adjacent rows across different writers.

Publication is bracketed around EmplaceWait. The consumer may start before the
producer logs push-end. The analyzer reports start-delay LOWER and UPPER bounds;
it never interprets that legitimate ordering as negative queue latency. Capture
boundaries can split requests, batches and frame lifetimes. Missing edges are
unknown, not zero. Reported PopWait may begin before T; capture totals clip it.
Overlapping waits must be merged rather than summed. Queue-ready and host CPU
data are evidence for the next investigation, not automatic causal attribution.

## Gates and reproduction

Production UWP Release/LTO/PGO USE compiles and links. The native policy fixture
checks arbitrary guest 501, selective events and unchanged legacy filtering.
The owner repository contains the offline analyzer and sixteen regression tests
covering partial captures, publication races, queue-local frame IDs, migration,
overlap accounting, fence identity reuse and elapsed-versus-CPU distinctions;
all 48 Python tests pass.

The GPU fixture uses real Device/Scheduler/Image download paths with byte-exact
65x47 RGBA data and a clean D3D12 debug layer, both inline and asynchronous. It
checks capture bounds, mode immutability and nested/cross-capture origin reset.
Analysis of both logs preserves original guest 501, software batch 37 and two
matching native ticks with no origin mismatch. These are fixture results, not
BOTW performance or a completed gameplay capture.

## First manual BOTW capture

120 vsyncs, 1995.260 ms, 66153 events, complete and below capacity. 48 frames
were queued. Original guest 83 owns 96 observed SubmitList requests; one partial
batch predates the capture. Native origins match. Inventory/B and scope/zoom
passed; the owner saved and returned to the library with Q.

The 48 waits of guest 79 are linked to actual nv-event objects, sync-object
wakes and the native fence callback/tick, not just matching timestamps. They
overlap 705.606 of 762.005 ms of software queue idle (92.5986%). Wake-to-resume
is only 0.034 ms median, 0.050 ms maximum. Guest 83 is the direct submitter,
but following only that thread would miss the dependency on guest 79.
Recorded dequeue waits total 182.992 ms; their observed overlap with these
queue-idle windows is zero. The recorder handoff median is 0.024 ms. Neither
number supports blaming the compositor or recorder dispatch for these gaps.

Separate waiters: the fence thread accounts for 757.037 ms of native fence
wait; the recording thread accounts for 50.022 ms. Their elapsed totals cannot
be added to frame duration. In 51 completed SubmitList batches longer than
5 ms, the first observed native submission has median lag 23.119 ms; in 46 it
arrives after 98% of the batch elapsed time. This supports investigating late
native submission/CPU-GPU overlap, not simply accelerating the ARM translator.
It does not prove when the GPU was busy or that earlier safe submission wins.

Example, relative capture ms: batch 4248 begins 59.696, tick 4116 is submitted
81.460, batch ends 81.589; guest 79 waits on the armed nv-event at 81.901. The
fence-thread wait ends 107.718, that fence's callback signals the same event at
108.113, the object wakes guest 79 at 108.115 and it resumes at 108.154. The
next batch from guest 83 starts 109.606. This is an observed dependency chain,
not permission to signal a fence early. Existing release/fence semantics stand.

Next hypothesis: bounded native submissions at safe command boundaries might
overlap GPU execution with remaining CPU preparation. This differs from .18 DMA
packet grouping: the relevant edge here is ExecuteCommandLists. It requires
state/lifetime/debug-layer gates and a same-executable performance comparison;
no partial-submission optimization is included in this diagnostic.

From the owner repository, run `tools/jadyx/run-cache-pressure-tests.ps1` with
`-TestName frame-chain-policy -OutputDirectory PRIVATE_GATE`, then
`tools/jadyx/run-transfer-tests.ps1` with
`-Mode FrameChain`, supplying `-JadyxRoot` for the external source checkout.
Analyze a saved log with `python tools/jadyx/analyze_frame_chain.py --log LOG
--out REPORT`; the fixture requires `--capture-id 1` because later captures test
context lifetime intentionally. Real gameplay capture is manual: load the same
save/view, press T once, leave the camera still until saving completes. Do not
send controls from a test script or compare instrumented FPS to normal .26.
