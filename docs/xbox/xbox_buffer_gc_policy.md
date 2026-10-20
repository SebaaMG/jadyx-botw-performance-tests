# Buffer GC and transfer pressure recovery

Private SeriesX candidate after .24. This change preserves the original renderer,
fences, data download paths, allocation-failure recovery and the 5120 MiB cache
budget. It does not enable DrawPlan or introduce a new JIT optimization.

The immediate-recovery condition restores the behavior of SeriesX patch0013:
low headroom still raises Critical/Emergency pressure, but only an exhausted app
or GPU budget requests a zero-reserve transfer-pool trim. Ordered hot transfers
retain the existing bounded reserve. A failed allocation still reclaims and retries.

D3D12 buffers now use `GetBufferGcPolicy` with independent app/GPU snapshots and
the existing pressure controller's hysteresis. Generic BufferCache does not compare
the old synthetic usage against thresholds based on a different budget while that
override is available. Vulkan and other runtimes retain the legacy selection path.
If no query has ever succeeded, the override is absent; after a successful sample,
missing domains retain the preceding pressure rather than silently relaxing it.

| Level | Minimum age, frames | Maximum visits | Maximum dirty downloads | Start-work budget |
| --- | ---: | ---: | ---: | ---: |
| Normal | 120 | 8 | 0 | 1000 us |
| Pressure | 120 | 32 | 1 | 1000 us |
| Critical | 60 | 64 | 1 | 1000 us |
| Emergency, below actual limits | 10 | 64 | 1 | 1000 us |
| Emergency, exhausted app/GPU budget | 10 | 64 | 64 | No time limit |

The time budget stops starting additional work; it cannot interrupt a download or
turn its completion into an asynchronous operation. Dirty entries deferred by a
budget remain tracked and alive. Before deleting one, the existing download path
still copies, finishes GPU work, and writes guest memory. DownloadBufferMemory
returns the number of guest bytes written for counters; its ordering is unchanged.
Age checks precede unsigned subtraction, preventing premature eviction at startup.

Buffer GC reports cumulative visits, evictions, deferred dirty entries, actual
downloads/bytes and elapsed times every300frames. Staging reports ring wait calls,
pressure-upload wait calls and pressure-readback wait calls separately, including
their elapsed times and the subset caused by failed allocations. Those are calls,
not necessarily blocking waits. They occur only on the recording thread; ordinary
cache hits do not add clocks or atomics. The legacy aggregate now says `staging
waits`, not `ring waits`. The conservative64MiB synchronous texture-GC fallback
is unchanged; this patch does not claim to eliminate all synchronous reclamation.

Native tests exercise the production policy and collector helper, including dirty
preservation, byte counters, visit/download/time bounds, startup ages, failures and
10000stress passes. Critical BufferCache, D3D12 and Vulkan objects compile against
the UWP configuration. These checks are not a gameplay FPS measurement or a new
GPU fence test. Final link, package and gameplay results are recorded separately
by the owner repository's candidate verification report.

Memory domains follow the contracts described in
[Microsoft QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo)
and the backend's existing texture pressure policy; app headroom is not DXGI usage.
No changes are submitted upstream. Original licenses and attribution remain intact.
