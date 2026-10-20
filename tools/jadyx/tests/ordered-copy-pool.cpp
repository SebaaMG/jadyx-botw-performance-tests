// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Production pool, real queue held behind a fence: reuse must not overwrite earlier copies.
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <d3d12sdklayers.h>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_cache_policy.h"
#include "video_core/renderer_d3d12/d3d12_copy_plan.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_transfer_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_uniform_cache_policy.h"
#include "video_core/buffer_cache/memory_tracker_base.h"

using namespace D3D12;
void Check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }

struct QueueBlock {
    ComPtr<ID3D12Fence> fence;
    ~QueueBlock() { if (fence) fence->Signal(1); }
    void Open() { ThrowIfFailed(fence->Signal(1), "unblock test queue"); }
};

void CheckDebug(ID3D12InfoQueue* info) {
    bool clean = true;
    for (u64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; info->GetMessage(i, nullptr, &size);
        std::vector<u8> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        info->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << message->pDescription << '\n'; clean = false;
        }
    }
    Check(clean, "D3D12 debug errors");
}

void Run(bool enabled, bool split, bool pressure) {
    SetOrderedCopyReuse(enabled);
    Device device;
    Scheduler scheduler{device};
    TransferBufferPool pool{device, scheduler};
    ComPtr<ID3D12InfoQueue> info;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&info)), "info queue");
    info->ClearStoredMessages();
    QueueBlock block;
    ThrowIfFailed(device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                          IID_PPV_ARGS(&block.fence)), "block fence");
    ThrowIfFailed(device.Queue()->Wait(block.fence.Get(), 1), "hold queue");
    constexpr u32 N = 96, SIZE = 1024;
    constexpr u64 MiB = 1024 * 1024;
    // Reproduce gameplay's pressure-policy input while the GPU cannot retire any
    // resource. Low headroom must not turn safe ordered reuse into repeated allocation.
    const CacheMemorySnapshot low_headroom{(5120 - 56)*MiB, 5120*MiB, 530*MiB, 3447*MiB};
    CachePressureController controller;
    Check(controller.Update(low_headroom) == CachePressure::Emergency,
          "56 MiB did not request proactive eviction");
    std::span<u8> source, result;
    auto upload = CreateMappedBuffer(device.Get(), SIZE * (N + 4), D3D12_HEAP_TYPE_UPLOAD, source);
    auto readback = CreateMappedBuffer(device.Get(), SIZE * (N + 4), D3D12_HEAP_TYPE_READBACK, result);
    for (u32 i = 0; i < source.size(); ++i) source[i] = u8((i * 37 + i / SIZE * 19) % 251);
    std::memset(result.data(), 0xcc, result.size());
    auto copy = [&](ComPtr<ID3D12Resource>& scratch, u32 index) {
        auto* cmd = scheduler.CommandList();
        cmd->CopyBufferRegion(scratch.Get(), 0, upload.Get(), index * SIZE, SIZE);
        TransitionResource(cmd, scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(readback.Get(), index * SIZE, scratch.Get(), 0, SIZE);
    };
    for (u32 i = 0; i < N; ++i) {
        auto scratch = pool.AcquireCopy(SIZE);
        copy(scratch, i);
        pool.ReleaseCopy(std::move(scratch));
        if (split && i % 7 == 6) scheduler.Flush();
        if (pressure && i % 5 == 4) {
            pool.TrimForPressure(controller.RequiresImmediateRecovery(low_headroom));
            Check(pool.PooledBytes() <= (enabled ? 4ULL << 20 : 0), "pressure reserve unbounded");
        }
    }
    const auto repeated = pool.GetStatistics();
    Check(enabled ? repeated.creates == 1 : repeated.creates == N,
          "copy-only creation count does not distinguish control and reuse");
    Check(!enabled || repeated.ordered_reuses == N - 1, "no in-flight ordered reuse");
    // Two live leases cannot alias, even while every GPU submission is still blocked.
    auto first = pool.AcquireCopy(SIZE), second = pool.AcquireCopy(SIZE);
    Check(first.Get() != second.Get(), "simultaneous leases alias");
    copy(first, N); copy(second, N + 1);
    pool.ReleaseCopy(std::move(first)); pool.ReleaseCopy(std::move(second));
    // Generic users retain their completion fence and cannot take pending copy-only scratch.
    const auto before_generic = pool.GetStatistics();
    auto generic = pool.Acquire(SIZE);
    Check(pool.GetStatistics().creates == before_generic.creates + 1,
          "generic pool bypassed completion fence");
    pool.Release(std::move(generic));
    // Off-owner acquisition is fenced too, even when the copy API was requested.
    const auto before_thread = pool.GetStatistics();
    std::exception_ptr error;
    std::thread worker([&] {
        try { auto b = pool.AcquireCopy(SIZE); pool.ReleaseCopy(std::move(b)); }
        catch (...) { error = std::current_exception(); }
    });
    worker.join(); if (error) std::rethrow_exception(error);
    Check(pool.GetStatistics().creates == before_thread.creates + 1,
          "off-owner copy bypassed completion fence");
    // Real OOM recovery can still empty everything, including pending ordered resources.
    pool.Trim(true);
    Check(pool.PooledBytes() == 0, "full trim kept scratch");
    auto after_trim = pool.AcquireCopy(SIZE);
    copy(after_trim, N + 2); pool.ReleaseCopy(std::move(after_trim));
    auto oversized = pool.AcquireCopy(8ULL << 20);
    copy(oversized, N + 3); pool.ReleaseCopy(std::move(oversized));
    pool.TrimForPressure(false);
    Check(pool.PooledBytes() <= (enabled ? 4ULL << 20 : 0), "large scratch retained under pressure");
    auto exhausted = low_headroom;
    exhausted.app_used = exhausted.app_limit;
    Check(controller.RequiresImmediateRecovery(exhausted), "exhaustion did not request recovery");
    pool.TrimForPressure(controller.RequiresImmediateRecovery(exhausted));
    Check(pool.PooledBytes() == 0, "immediate recovery kept reserve");
    block.Open();
    scheduler.Finish();
    Check(std::memcmp(source.data(), result.data(), SIZE * (N + 4)) == 0,
          "in-flight scratch overwrote earlier bytes");
    CheckDebug(info.Get());
    std::cout << "pool enabled=" << enabled << " split=" << split << " pressure=" << pressure
              << " free_MiB=" << low_headroom.AppFree() / MiB
              << " repeated creates=" << repeated.creates << " reuse=" << repeated.ordered_reuses
              << " compared=" << SIZE * (N + 4) << " bytes\n";
}

void Plans() {
    Device device;
    CopyFootprintCache cache;
    for (u32 round = 0; round < 3; ++round) {
        for (u32 shape = 0; shape < 7; ++shape) {
            D3D12_RESOURCE_DESC desc{
                .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 32 + shape * 4,
                .Height = 17 + shape, .DepthOrArraySize = 3, .MipLevels = 3,
                .Format = shape % 2 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM,
                .SampleDesc = {.Count = 1}, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN};
            CopyFootprintCache::Plan fallback, original;
            const auto& plan = cache.Get(device.Get(), desc, true, fallback);
            const auto& control = cache.Get(device.Get(), desc, false, original);
            Check(plan.bytes == control.bytes && plan.footprints.size() == control.footprints.size(),
                  "cached footprint shape mismatch");
            for (u32 i = 0; i < plan.footprints.size(); ++i) {
                const auto& a = plan.footprints[i]; const auto& b = control.footprints[i];
                Check(a.Offset == b.Offset && a.Footprint.Format == b.Footprint.Format &&
                      a.Footprint.Width == b.Footprint.Width && a.Footprint.Height == b.Footprint.Height &&
                      a.Footprint.Depth == b.Footprint.Depth && a.Footprint.RowPitch == b.Footprint.RowPitch,
                      "cached footprint bytes mismatch");
            }
            const auto before = cache.GetStatistics();
            (void)cache.Get(device.Get(), desc, true, fallback);
            Check(cache.GetStatistics().hits == before.hits + 1 &&
                  cache.GetStatistics().builds == before.builds, "layout cache missed stable shape");
        }
    }
    std::cout << "footprints: 21 comparisons, 21 immediate hits, eviction and format changes pass\n";
}

/// Real dirty-page tracker: the adapter records whether a CPU page would notify a new write.
struct PageTracking {
    static constexpr bool HAS_FLUSH_INVALIDATION = true;
    s32 cached[4]{};
    void UpdatePagesCachedCount(u64 address, size_t size, s32 delta) {
        for (u64 p = address / 4096; p < (address + size + 4095) / 4096; ++p)
            if (p < 4) cached[p] += delta;
    }
    void UpdatePagesCachedBatch(std::span<const std::pair<u64, size_t>> ranges, s32 delta) {
        for (const auto& [address, size] : ranges) UpdatePagesCachedCount(address, size, delta);
    }
};

void Uniforms(bool enabled, bool skip_clean_sync = false) {
    Device device;
    Scheduler scheduler{device};
    StagingBufferPool staging{device, scheduler};
    UniformCachePolicy policy;
    PageTracking pages;
    VideoCommon::MemoryTrackerBase<PageTracking> tracker{pages};
    ComPtr<ID3D12InfoQueue> info;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&info)), "uniform info queue");
    info->ClearStoredMessages();
    constexpr u64 Address = 4096, Page = 4096, Size = 256, Draws = 600;
    std::array<u8, Page> cpu{};
    std::vector<u8> expected(Size * Draws);
    std::span<u8> output;
    auto readback = CreateMappedBuffer(device.Get(), expected.size(), D3D12_HEAP_TYPE_READBACK, output);
    auto cached = CreateCommittedBuffer(device.Get(), Page, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE, "coherent uniform gate");
    tracker.MarkRegionAsCpuModified(Address, Page);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    u64 upload_bytes = 0, stream_bytes = 0, host_uses = 0, sync_skips = 0;
    const auto write = [&](u32 seed) {
        // An untracked dirty page does not notify a second write. This is the reason a
        // snapshot cache keyed only by WriteMemory callbacks would be incorrect.
        if (pages.cached[1] > 0) tracker.MarkRegionAsCpuModified(Address, Page);
        for (u32 j = 0; j < cpu.size(); ++j) cpu[j] = u8((seed * 17 + j * 3) % 251);
    };
    write(1);
    for (u32 draw = 0; draw < Draws; ++draw) {
        // Stable -> every-draw changes -> stable, followed by several DMA-like whole-page
        // invalidations. Tests policy backoff and re-promotion, with actual dirty tracking.
        if ((draw >= 160 && draw < 240) || draw == 410 || draw == 500) write(draw + 1);
        const u64 identity = draw < 500 ? 1 : 2; // a remap/backing identity must restart the probe
        const bool dirty = tracker.IsRegionCpuModified(Address, Size);
        const bool use_host = enabled && policy.PreferCached(Address, Size, identity, draw / 4, dirty);
        ID3D12Resource* source{}; u64 offset{};
        if (use_host) {
            if (!skip_clean_sync || dirty) tracker.ForEachUploadRange(Address, Size, [&](u64 begin, u64 length) {
                auto upload = staging.Request(length, MemoryUsage::Upload);
                std::memcpy(upload.mapped_span.data(), cpu.data() + begin - Address, length);
                TransitionResource(scheduler.CommandList(), cached.Get(), state,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
                scheduler.CommandList()->CopyBufferRegion(cached.Get(), begin - Address,
                    upload.buffer, upload.offset, length);
                state = D3D12_RESOURCE_STATE_COPY_DEST;
                upload_bytes += length;
            });
            else ++sync_skips;
            if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
                TransitionResource(scheduler.CommandList(), cached.Get(), state,
                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
                state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            }
            source = cached.Get(); ++host_uses;
        } else {
            auto upload = staging.Request(Size, MemoryUsage::Upload);
            std::memcpy(upload.mapped_span.data(), cpu.data(), Size);
            source = upload.buffer; offset = upload.offset; stream_bytes += Size;
        }
        scheduler.CommandList()->CopyBufferRegion(readback.Get(), draw * Size, source, offset, Size);
        std::memcpy(expected.data() + draw * Size, cpu.data(), Size);
    }
    scheduler.Finish();
    Check(std::memcmp(expected.data(), output.data(), expected.size()) == 0,
          "coherent policy retained stale CPU/DMA/untracked uniform bytes");
    if (enabled) {
        const auto s = policy.GetStatistics();
        Check(host_uses > 300 && s.backoffs > 0 && s.probes > 1,
              "uniform stable/dynamic transitions were not exercised");
        Check(upload_bytes + stream_bytes < Size * Draws, "uniform trace did not reduce copied bytes");
        if (skip_clean_sync) Check(sync_skips > 300, "clean upload walk skip was not exercised");
    }
    CheckDebug(info.Get());
    std::cout << "uniform enabled=" << enabled << " skip_clean_sync=" << skip_clean_sync
              << " sync_skips=" << sync_skips << " host uses=" << host_uses
              << " uploaded=" << upload_bytes << " streamed=" << stream_bytes
              << " compared=" << expected.size() << " bytes\n";
}

int main(int argc, char**) try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    SetAsyncRecording(argc > 1);
    for (bool enabled : {false, true})
        for (bool split : {false, true})
            for (bool pressure : {false, true}) Run(enabled, split, pressure);
    Plans();
    Uniforms(false); Uniforms(true); Uniforms(true, true);
    std::cout << "PASS: ordered-copy pool, exact bytes, pressure and D3D12 debug clean\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
