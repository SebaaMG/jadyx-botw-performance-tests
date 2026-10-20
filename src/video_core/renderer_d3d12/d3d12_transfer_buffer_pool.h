// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <mutex>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;

/// Opt-in SeriesX copy-only scratch reuse. Configure before constructing the renderer.
void SetOrderedCopyReuse(bool enabled);

/// Reuses the DEFAULT-heap buffers that copies go through (texture reinterpretations, plane
/// splits, row repacks; copies within one cached buffer) instead of creating a committed
/// resource for every copy and layer. One pool, owned by the renderer, serves both caches.
///
/// A released buffer is handed out again once the GPU has finished the submission that used it.
/// It is in COMMON then, like a new one: buffers decay to COMMON at the end of every
/// ExecuteCommandLists. Callers therefore keep the barriers they record for a fresh buffer.
class TransferBufferPool {
public:
    explicit TransferBufferPool(const Device& device, Scheduler& scheduler);
    ~TransferBufferPool();

    TransferBufferPool(const TransferBufferPool&) = delete;
    TransferBufferPool& operator=(const TransferBufferPool&) = delete;

    /// A buffer of at least size bytes in COMMON (with UAV access when unordered_access).
    [[nodiscard]] ComPtr<ID3D12Resource> Acquire(u64 size, bool unordered_access = false);

    /// Copy-only scratch: all accesses must be recorded on this scheduler's direct queue.
    /// ReleaseCopy must follow the LAST read, in COPY_SOURCE, before another AcquireCopy.
    /// Never expose this buffer to the CPU or another queue. A live lease cannot be reused.
    [[nodiscard]] ComPtr<ID3D12Resource> AcquireCopy(u64 size);
    void ReleaseCopy(ComPtr<ID3D12Resource>&& buffer);

    /// Returns a buffer once the commands recorded so far are done with it. Buffers past the pool's
    /// budget are released instead.
    void Release(ComPtr<ID3D12Resource>&& buffer);

    /// Frees idle buffers: all of them under memory pressure, otherwise the ones unused for a while.
    void Trim(bool under_pressure);

    /// Pressure trim keeps at most 4 MiB of recently used ordered-copy scratch. Actual OOM
    /// still uses Trim(true), which releases everything, and retains the existing retry.
    void TrimForPressure(bool immediate_recovery);

    struct Statistics {
        u64 requests{}, completed_reuses{}, ordered_reuses{}, creates{}, busy_skips{};
        u64 pressure_trims{}, full_trims{}, trimmed_bytes{}, pooled_bytes{}, peak_pooled_bytes{};
    };
    [[nodiscard]] Statistics GetStatistics() const;
    /// Cheap counters; emits one aggregate per 300 texture-cache frames, never raw guest data.
    void TickStatistics();

    [[nodiscard]] u64 PooledBytes() const;

    /// After the commit limit refused a resource of size bytes: frees what finished work no longer
    /// needs, so the caller can try once more. False (nothing freed) off the recording thread.
    bool ReclaimAfterOutOfMemory(u64 size, const char* what);

    /// What else to free when the commit limit refuses a buffer, after the pool's own buffers and
    /// the finished work's deferred releases (the texture cache's emptied heaps).
    void SetReclaimer(std::function<void()> reclaimer_) {
        reclaimer = std::move(reclaimer_);
    }

private:
    struct Entry {
        ComPtr<ID3D12Resource> buffer;
        u64 size;
        bool unordered_access;
        u64 tick; ///< scheduler tick of the last submission that used it
        bool ordered_copy{}; ///< final recorded access was COPY_SOURCE on this direct queue
    };

    /// Idle buffers above this are released: the pool must not grow into the texture budget.
    static constexpr u64 MAX_POOLED_BYTES = 64ULL << 20;
    /// Submissions an idle buffer is kept for when there is no memory pressure.
    static constexpr u64 IDLE_TICKS = 2048;
    static constexpr u64 PRESSURE_RESERVE = 4ULL << 20;
    static constexpr u64 HOT_TICKS = 4;

    ComPtr<ID3D12Resource> AcquireImpl(u64 size, bool unordered_access, bool copy_only);
    void ReleaseImpl(ComPtr<ID3D12Resource>&& buffer, bool ordered_copy);
    void TrimImpl(bool under_pressure, u64 reserve);

    const Device& device;
    Scheduler& scheduler;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    u64 pooled_bytes{};
    std::function<void()> reclaimer;
    bool ordered_copy_reuse{};
    Statistics statistics;
    u32 report_frames{};
};

} // namespace D3D12
