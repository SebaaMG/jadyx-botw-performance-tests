# SeriesX .29: continuous native delivery

Extends .28 without a per-frame submission quota. continuous_submit opts in;
partial_submit remains enabled. Minimum completed work and sampled clocks are
preserved. If no issued scheduler ticks remain unfinished, the interval is2ms;
with one unfinished tick it is4ms. Two unfinished ticks defer optional delivery.
Required guest/fence/recovery submissions keep their existing behavior.

CurrentTick-1 includes recorded submissions, SubmittedTick identifies native
queue delivery, and KnownGpuTick identifies completion. Admission counts all
unfinished issued work; diagnostics separately count recorder/native backlog.
No CPU wait is added to the admission policy. The interval and minimum work
bound the submission rate without stopping supply after the third cut.

coalesce_fence_flush independently enables a callback after a real
InnerFence::Queue successfully records its Scheduler::Flush. Rasterizer clears
only the already-submitted draw counter at that boundary. Query closing,
copy/readback commands and the original fence Signal remain in that flush.
Stub fences and failed submissions do not notify. PendingUploadBytes compares
its tick with CurrentTick, so later uploads are still detected. The notification
counter is not a count of measured avoided submissions.

The callback only assigns producer state. With an async recorder it means the
submission was recorded, not completed by the GPU. Clearing from every generic
on_reset would be incorrect: resets can happen during draw preparation before
the actual draw is emitted. The callback is limited to the fence boundary.

The same executable offers .28 behavior, continuous-only, coalescence-only and
combined configurations. Keep .26 readback recovery enabled in all variants.
The already-qualified .28 binaries/symbols/package are archived privately.

Continuous mode also removes the artificial dependency-depth96 blocking read
from D3D12 counters. Completed prefixes are folded without waiting, while an
unresolved suffix retains its resources and exact accumulated result. Reading
and releasing a long chain are iterative. BlockingQuery can reenter the cache
through a flush/reset, so the outer read rechecks a cached inclusive result and
reads the base only after the callback returns. The legacy path is unchanged.
PendingReport still supports four unresolved slices and keeps its safe fallback;
this change does not claim to eliminate every conditional-rendering CPU wait.

Selective T diagnostics describe every scheduler list by completed rasterizer
operations and staging upload bytes, plus native Close and allocator/reset time.
Native command-list storage is opaque: these are not its byte size or all D3D12
commands. Timers, metadata callbacks and completion sampling run only in a
frame-chain capture; normal play adds only an operation counter and the existing
frame-boundary marker. Prior submitted/completed scheduler ticks are sampled
under QueueMutex immediately before ExecuteCommandLists. A drained scheduler is
not proof that the entire physical queue is idle: the presenter also uses it.
TickFrame boundaries let the analyzer group issued lists per scheduler frame,
including async submissions that arrive after the next CPU frame boundary.

Policy gate passes512continuous deliveries without a frame reset. Owner51Python
tests pass, including missing metadata and upload-only lists. Final UWP
Release/LTO/PGO-USE build71operations passes. Both legacy and expanded GPU
gates pass inline and async, with exact bytes/counters and no D3D12 warnings or
errors. No .29 FPS measured. The initial package selects continuous-only;
coalescence is implemented but starts off to measure it independently next.

The expanded native gate uses production QueryCache/HostCounter, real queue
blocking, exact GPU results, conditional predicates, shared report snapshots,
chains beyond96, reentrant reads and iterative destruction. Report registration
is white-box; the public guest Query/channel path is not covered. Fence tests
include query resolve and on-submit/reset callbacks without drawings, a fresh
8MiB upload after a fence, stubs, and a synthetic submit callback failure. They
test InnerFence and the producer boundary rather than a complete guest session.

Build/test notes: the extra fence-manager include exposed a fixture-only global
Result collision with the core Result type. The fixture now uses UniformGateResult.
A second build attempt could not open a log held by the preceding link; it did
not start Ninja. Builds are serialized and the following18-operation build passed.
The expanded fixture initially redefined main through a nested test include;
an explicit no-main guard fixes it. Its first runtime failure was also in the
fixture: equal12/12 page geometry disables the sparse small-page table, then
Map(false) used the absent table. Normal16/12 geometry and BindInterface correct
the setup. Both native modes pass after this fixture-only correction; no
production memory-manager workaround was added.

References:
- https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists
- https://gpuopen.com/learn/rdna-performance-guide/
- xbox_partial_submission.md: .28 measured improvement and saturated quota.
