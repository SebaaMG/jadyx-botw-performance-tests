# SeriesX readback recovery candidate

Private source adaptation on the .25 tree. The .17 and .25 executable references
remain archived. No gameplay speedup is claimed by the implementation gates.

The readback staging pool retains at most 8 MiB of allocations between 64 KiB and
2 MiB. Admission adapts to one eighth of measured free application memory, up to
that cap; it does not allocate anything simply to fill a cache. Charges use 64 KiB
allocation granularity and include pinned/in-flight entries. Entries used once
expire after 8 frame ticks; repeatedly used entries after 120. Budget reductions
reclaim completed, unpinned entries starting with the least reused and oldest.
Pinned/in-flight charges may exceed a newly reduced target until safe retirement,
but cannot admit new retained allocations in that state. Forced recovery can release every
completed unpinned entry. Deferred CPU owners and GPU completion remain required.
Pressure misses wait for an eligible size bucket's submitted fence; they do not
wait for unrelated queue tails. Ordinary pressure preserves the bounded reserve.
Source counters distinguish buffer, texture, texture-GC and other requests.

The GPU regression blocks the queue while testing 48, 64, 80 and 128 MiB of
application headroom. Ordinary pressure must prepare four images without pool
waits or texture GC waits. A separate test holds an unrelated later submission
blocked and requires a compatible buffer to become reusable before that tail.
The test waits for recorder submission before inserting its queue barriers, so
the same queue ordering is exercised with inline and worker-thread recording.

FrameTrace adds staging-wait-request (a=request bytes, b=fence),
staging-wait-reason (a=ring/upload pressure/readback pressure as 0/1/2, bit 8=OOM;
b=other/buffer/texture/texture-GC as 0/1/2/3), staging-wait-long (a=elapsed us,
b=fence), and texture-gc-wait-request (a=reserved bytes, b=fence).
texture-gc-sync-reason value 4 identifies an exhausted-budget batch wait.
These events are inactive outside a requested trace; no per-draw tracing is added.

Staging maintenance runs on the cache frame tick; its duplicate call from Present
is removed. Readback cache changes are protected by a recursive mutex, independent
of the existing buffer and texture cache mutexes.

Image downloads coalesce row copies only when row pitches and slice sizes agree.
The original per-row path remains for padding, with the existing transition and
resource retirement. Counters record actual buffer-copy commands avoided.

Texture GC looks ahead through at most four eligible dirty images. Readbacks are
still bounded by the existing 8 MiB pending budget, checked against the capacity
of a reused allocation too. Copies share one submission; the GPU thread publishes
completed data later. Only actual budget exhaustion waits synchronously as a batch.
Large or unsupported images retain the original recovery path. Guest CPU reads,
syncpoints, alias order, version checks and stale readback cancellation are retained.
This is GC eviction lookahead, not Ryujinx-style anticipation of recurring guest
CPU reads. That separate mechanism needs texture access history and additional
ordering tests before enabling it. No per-texture CPU-demand history is claimed.
Device capability logging already queries UMA and CacheCoherentUMA. No relaxed
pitch alignment or coherent-memory behavior is assumed by these changes.
OPTIONS13 copy-pitch support is now queried and logged without enabling a new
copy path. Query failure remains unsupported, not an inferred platform capability.
The reserve and pending budget are not two independent charges on a retained
GC allocation: its capacity is included in both accounting views. Non-retained
large GC allocations still count against the pending budget, so an 8 MiB total
readback-memory cap is not claimed. The GPU gate covers overflow deferral,
stale GPU-version replacement and CPU-modification cancellation explicitly.

Guest backing maintenance is adapted from JulianDr14/jadyx commit
28d5b90e6b8473cd0ffe20a6458ceb6d9b5434d3: grouped optimized-page initialization,
kernel-free block decommit, and protected zero-page scanning. The kernel-free pass
is enabled; scanning remains opt-in with host_trim=1 for separate cost validation.
pool_trim=0 disables free-block maintenance. Host memory implementation and the
optimized-page helper retain their upstream attribution.

Texture heap growth adapts commit 9b17c0300fec1be650afa9c1abc2793fd62c422e to the
existing buffer-heap extension. Pending allocations are retired before growth,
then heaps use the minimum required aligned size under app/GPU pressure. Normal
texture and buffer block sizes remain 64 MiB and 4 MiB. Pool lookups check heap
alignment and reserve ranges while holding the allocator lock.

Comparison switches are readback_reuse, readback_copy_coalesce, gc_readback_batch
and pool_trim. They are configuration-only and do not add frontend menus. The
candidate keeps DrawPlan and detailed profilers off. Native helper tests and the
production-library GPU gate cover retained bytes, CPU pins, blocked GPU work,
specific fences, exact copied bytes, compacted footprints and grouped downloads.

Upstream sources and documentation:

- https://github.com/JulianDr14/jadyx/commit/28d5b90e6b8473cd0ffe20a6458ceb6d9b5434d3
- https://github.com/JulianDr14/jadyx/commit/9b17c0300fec1be650afa9c1abc2793fd62c422e
- https://github.com/blackbearreloaded/ProsperoEden/blob/a26ba788222465ec7fb94dc813958fc749734e07/headless/vulkan_gc_downloads.inc
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps
- https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data
