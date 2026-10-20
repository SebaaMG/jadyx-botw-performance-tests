# SeriesX .30: event contracts and consumer completion

Extends the qualified .29 configuration, including continuous_submit and
coalesce_fence_flush. Three independent boot options default off in source:
nonblocking_nv_events, consumer_release_fences, detached_syncpoint_actions.
The private .30 package will opt in after the native gates pass. No .30 gameplay
or FPS result exists yet. Original firmware, keys and decompiler output remain
outside Git; this implementation is ordinary host HLE, not firmware execution.

## Evidence and event behavior

Focused analysis of the owner's firmware 22.5.0 resolves the nvservices ioctl
dispatcher at module offset 0x5b4d0 and the shared EventWait routine at 0x5f2c0.
The timeout-zero branch at 0x5f3a0 returns internal -11 before ticket allocation
or arming. The async ioctl preserves its input value; the allocation variant
clears that output. Cached and refreshed expiry checks still precede this
branch. Internal pending -11 and actual timeout -110 both map to NvResult 5.
The private evidence verifier checks 74 instructions, 12 relocated pointers,
three errno mappings, two device names and three module segment hashes.

Cancellation at 0x5f670 removes a subscription and can join its own executing
callback. It does not prove a failed GPU dependency and does not globally stop
the guest until the threshold is reached. Existing fails counts cancellations
per slot, not hardware errors. After repeated cancellation, the opt-in HLE path
arms the ordinary event and requests coalesced, nonblocking GPU progress instead
of StallApplication/WaitHost. Zero-timeout polls never allocate an event or
enter that recovery. Finite and infinite real waits retain their event-based
contract. Invalid syncpoint IDs are still rejected before polling.

Event cancellation joins the specific callback before clearing or recycling the
event. Completed/cancelled action handles stay valid; cancellation of an older
generation cannot signal a later generation. The legacy global fallback remains
available with the option off and now reports its timeout, cancellation count
and duration. Historical .29 fallback warnings were outside its four selected
measurement windows; this correction cannot be credited with saving their mean.

## Framebuffer dependency

HardwareComposer keeps a consumer fence with each acquired frame. BufferQueue
returns that fence to the next producer rather than recycling its old acquire
dependency. Invalid fence counts and stale slots must not mutate the buffer.
The display allocates one dedicated client-managed syncpoint, and each
composition carries its exact reserved target, not an anonymous increment.

D3D12 attaches a real delayed fence to the existing presentation render-list
submission, after its last source-image read and before private presentation
work. This remains real even with Low accuracy/Immediate guest-fence settings.
There is no extra empty submission or CPU wait per frame. Guest logical
completion is published only after the renderer returns; host completion is
published only when that native fence finishes. Ordinary guest syncpoint
signals keep their existing behavior. Non-D3D12 backends do not advertise this
capability and retain their previous path.

Compositions can finish out of order. CompleteGuest/CompleteHost therefore keep
completed targets and advance only the contiguous frontier. Completing target 2
before target 1 must not release frame 1. Duplicate completion is harmless;
comparisons use signed modular differences for 32-bit wrap. The dedicated
consumer stream must not mix anonymous increments with explicit completion.
These queues dispatch ready callbacks outside the global lock even when the
broader detached option is off, so independent switches do not introduce a
consumer completion deadlock. Pulse keeps the optional allocated display ID;
older empty display payloads remain accepted.

Source-buffer safety is distinct from swap-interval/hold policy and from a
presenter's private copy. The change does not skip producer dependencies,
remove GPU barriers, alter WFI or change the legal presentation cadence.

## Callback ownership

Syncpoint actions have stable shared identities and Pending/Running/Completed/
Cancelled states rather than list iterators that can become dangling. New-mode
dispatch removes ready work under the global mutex, runs it on the calling
thread outside that mutex, and serializes each individual action queue.
Unrelated syncpoints can progress while a composition callback is slow.
Cancellation waits only for the selected running action; self-cancellation and
reentrant signaling are supported. A stack-local TLS chain recognizes every
active ancestor, so a nested callback never joins its own caller. Already-ready
callbacks use the same per-queue serialization in detached mode and recheck
shutdown after acquiring it; legacy immediate dispatch is preserved for control.
Shutdown wakes blocking waits and cancels
queued work. Owners must still retire/join their running actions before freeing
captured state. The mutex is not replaced by an unbounded background executor.

Selective T events distinguish mutex acquisition, callback duration and whether
the callback ran outside the global lock, nonblocking recovery requests, and
consumer-fence request/queue/physical completion. Timers stay off in normal
play. Capture-edge requests without a matching event are reported as unknown,
not zero latency. Completed physical targets may precede frontier availability.

## Validation checkpoint

UWP Release/LTO/PGO-USE compilation passes; the final Ninja dry run has no work.
Owner Python tests: 54 pass. FirmwareSync passes inline and async with actual
core/NV/BufferQueue/FenceManager and D3D12 Scheduler. Idle cache fixtures contain
no work; this is not a complete game renderer. Its original 12 KiB NRO uses
private empty test paths, no keys or commercial game. The initial fixture did
not start the GPU/CPU-ready handshake and then used the AsyncWait output as a
QueryEvent identifier. Both fixture errors are corrected; QueryEvent production
ABI is unchanged, and the test inspects its registered event via PulseEvent.
The progress request must enqueue TickGPU(true) even in synchronous emulation:
TickGPU(is_async) would still block its caller behind existing GPU-thread work.
The held-work native test covers that distinction.

Passing checks include 10,000 pending polls, event cancel/rearm generations,
finite/infinite waits, completed polls, unrelated progress under a held callback,
stable cancellation, shutdown, concurrent streams, wrap, out-of-order frames,
real ReleaseBuffer/DequeueBuffer, a GPU-held source copy, and release while
unrelated private presentation work remains blocked. Run inline and async, plus
the existing ContinuousSubmit regression gate before delivery. The two new gate
modes have exact 4,096-byte output and clean D3D12 debug. Cancellation while its
callback is blocked in kernel signaling also passes: clear must join that exact
callback before clearing the event. These are correctness checks, not FPS data.
Review found the two reentrancy cases above; both are fixed and covered by the
expanded gate. Final UWP incremental build v6 and FirmwareSync/ContinuousSubmit
gates pass inline/async, with exact results and clean D3D12 debug. The static
follow-up review found no additional blocker for the all-on candidate. This is
not a whole-emulator concurrency proof. Final EXE SHA-256:
ea05f8e531d47d60850c502218be64a4dba4811e6003686629d104430446910c.
Source export and packaging follow these gates. No gameplay or FPS .30 result
exists; private delivery status and later measurements belong to owner evidence.

For manual A/B, preserve .29 readback recovery, continuous delivery, coalescence,
memory budgets and shader caches. Compare the same .30 executable with the
three options independently selectable, normal captures separate from T.
Reference .29 combined: 33.34667 ms/frame, 29.98800 FPS in 1,200 frames. A
30-FPS-capped scene may reveal improvement in active work or tails rather than
a higher displayed FPS. Do not add overlapping timing scopes as exclusive cost.
