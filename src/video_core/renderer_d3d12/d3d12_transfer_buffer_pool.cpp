// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"

namespace D3D12 {

namespace {

/// Buffers are placed at 64 KiB anyway: sizing them so lets similar copies share one.
constexpr u64 SIZE_GRANULARITY = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
std::atomic_bool ordered_copy_reuse_enabled{};

} // Anonymous namespace

void SetOrderedCopyReuse(bool enabled) {
    ordered_copy_reuse_enabled.store(enabled, std::memory_order_relaxed);
}

TransferBufferPool::TransferBufferPool(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_},
      ordered_copy_reuse{ordered_copy_reuse_enabled.load(std::memory_order_relaxed)} {}

TransferBufferPool::~TransferBufferPool() = default;

ComPtr<ID3D12Resource> TransferBufferPool::Acquire(u64 size, bool unordered_access) {
    return AcquireImpl(size, unordered_access, false);
}

ComPtr<ID3D12Resource> TransferBufferPool::AcquireCopy(u64 size) {
    return AcquireImpl(size, false, ordered_copy_reuse && scheduler.IsRecordingThread());
}

ComPtr<ID3D12Resource> TransferBufferPool::AcquireImpl(u64 size, bool unordered_access,
                                                    bool copy_only) {
    size = Common::AlignUp(std::max<u64>(size, 1), SIZE_GRANULARITY);
    {
        std::scoped_lock lock{mutex};
        ++statistics.requests;
        // Best fit among the idle buffers, and at most twice the size so a large buffer is not
        // tied up by small copies.
        auto best = entries.end();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->size < size || it->size > size * 2 ||
                (unordered_access && !it->unordered_access)) {
                continue;
            }
            // Only ordered copy leases can bypass the completion fence. ReleaseCopy follows
            // all reads; the next writes are recorded later on the same direct queue.
            if (!(copy_only && it->ordered_copy) && !scheduler.IsFree(it->tick)) {
                ++statistics.busy_skips;
                continue;
            }
            if (best == entries.end() || it->size < best->size) {
                best = it;
            }
        }
        if (best != entries.end()) {
            if (copy_only && best->ordered_copy && best->tick == scheduler.CurrentTick()) {
                // Reuse inside a submission: the previous reads must finish before overwriting.
                // Across submissions a buffer decays to COMMON, so promotion suffices. No
                // unconditional transition to COMMON at ReleaseCopy (which stalls unnecessarily).
                const D3D12_RESOURCE_BARRIER barrier{
                    .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                    .Transition = {best->buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST}};
                scheduler.CommandList()->ResourceBarrier(1, &barrier);
            }
            if (copy_only && best->ordered_copy && !scheduler.IsKnownFree(best->tick)) {
                ++statistics.ordered_reuses;
            } else {
                ++statistics.completed_reuses;
            }
            ComPtr<ID3D12Resource> buffer = std::move(best->buffer);
            pooled_bytes -= best->size;
            *best = std::move(entries.back());
            entries.pop_back();
            return buffer;
        }
    }
    HRESULT hr = S_OK;
    const auto create = [&] {
        return CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATE_COMMON,
                                     unordered_access ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                      : D3D12_RESOURCE_FLAG_NONE,
                                     "Create transfer buffer", &hr);
    };
    ComPtr<ID3D12Resource> buffer = create();
    if (!buffer && hr == E_OUTOFMEMORY && ReclaimAfterOutOfMemory(size, "transfer buffer")) {
        buffer = create();
        if (buffer) {
            LOG_INFO(Render, "D3D12: transfer buffer created after reclaiming");
        }
    }
    if (!buffer) {
        ThrowIfFailed(hr, "Create transfer buffer");
    }
    { std::scoped_lock lock{mutex}; ++statistics.creates; }
    return buffer;
}

bool TransferBufferPool::ReclaimAfterOutOfMemory(u64 size, const char* what) {
    if (!scheduler.IsRecordingThread()) {
        return false;
    }
    // The commit limit refused it, typically while a loading screen fills the caches. Closing
    // the game for it lost the session; give back what finished work no longer needs (idle
    // pooled buffers, deferred releases, then the heaps they emptied). Waiting only on
    // submitted ticks never splits the list being recorded.
    LOG_WARNING(Render, "D3D12: out of memory creating a {} byte {}; reclaiming and retrying",
                size, what);
    Trim(true);
    const u64 current = scheduler.CurrentTick();
    if (current > 1) {
        scheduler.Wait(current - 1);
    }
    scheduler.CollectGarbage();
    if (reclaimer) {
        reclaimer();
    }
    return true;
}

void TransferBufferPool::Release(ComPtr<ID3D12Resource>&& buffer) {
    ReleaseImpl(std::move(buffer), false);
}

void TransferBufferPool::ReleaseCopy(ComPtr<ID3D12Resource>&& buffer) {
    const bool ordered = ordered_copy_reuse && scheduler.IsRecordingThread();
    ReleaseImpl(std::move(buffer), ordered);
}

void TransferBufferPool::ReleaseImpl(ComPtr<ID3D12Resource>&& buffer, bool ordered_copy) {
    if (!buffer) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = buffer->GetDesc();
    {
        std::scoped_lock lock{mutex};
        if (pooled_bytes + desc.Width <= MAX_POOLED_BYTES) {
            pooled_bytes += desc.Width;
            statistics.peak_pooled_bytes = std::max(statistics.peak_pooled_bytes, pooled_bytes);
            entries.push_back({
                .buffer = std::move(buffer),
                .size = desc.Width,
                .unordered_access = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0,
                .tick = scheduler.CurrentTick(),
                .ordered_copy = ordered_copy,
            });
            return;
        }
    }
    scheduler.DeferRelease(std::move(buffer));
}

void TransferBufferPool::Trim(bool under_pressure) {
    TrimImpl(under_pressure, 0);
}

void TransferBufferPool::TrimForPressure(bool immediate_recovery) {
    TrimImpl(true, ordered_copy_reuse && !immediate_recovery ? PRESSURE_RESERVE : 0);
}

void TransferBufferPool::TrimImpl(bool under_pressure, u64 reserve) {
    const u64 current = scheduler.CurrentTick();
    std::scoped_lock lock{mutex};
    if (under_pressure) {
        ++statistics.pressure_trims;
        if (reserve == 0) ++statistics.full_trims;
    }
    // Prefer keeping the smallest hot entries; the total is bounded, including in-flight ones.
    u64 retained = 0;
    if (reserve) {
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.size < b.size;
        });
    }
    std::erase_if(entries, [&](Entry& entry) {
        const bool idle_too_long = current > entry.tick + IDLE_TICKS;
        if (!under_pressure && !idle_too_long) {
            return false;
        }
        const bool hot = current >= entry.tick && current - entry.tick <= HOT_TICKS;
        if (reserve && entry.ordered_copy && hot && entry.size <= reserve - retained) {
            retained += entry.size;
            return false;
        }
        pooled_bytes -= entry.size;
        statistics.trimmed_bytes += entry.size;
        // Not necessarily done on the GPU yet: release it after the work recorded so far.
        scheduler.DeferRelease(std::move(entry.buffer));
        return true;
    });
}

TransferBufferPool::Statistics TransferBufferPool::GetStatistics() const {
    std::scoped_lock lock{mutex};
    auto result = statistics;
    result.pooled_bytes = pooled_bytes;
    return result;
}

void TransferBufferPool::TickStatistics() {
    if (++report_frames % 300 != 0) return;
    const auto s = GetStatistics();
    LOG_INFO(Render, "SeriesX transfer pool: requests {}, completed reuse {}, ordered reuse {}, "
             "creates {}, busy skips {}, pressure trims {} (full {}), trimmed {} KiB, pooled "
             "{} KiB, peak pooled {} KiB; ordered {} (cumulative)", s.requests,
             s.completed_reuses, s.ordered_reuses, s.creates, s.busy_skips, s.pressure_trims,
             s.full_trims, s.trimmed_bytes >> 10, s.pooled_bytes >> 10,
             s.peak_pooled_bytes >> 10, ordered_copy_reuse);
}

u64 TransferBufferPool::PooledBytes() const {
    std::scoped_lock lock{mutex};
    return pooled_bytes;
}

} // namespace D3D12
