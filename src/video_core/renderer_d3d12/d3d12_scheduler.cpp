// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/frame_trace.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_partial_submit_scope.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace D3D12 {

namespace {

/// One auto-reset event per waiting thread. A shared event is unsafe: the GPU thread and the fence
/// thread may wait at the same time, and one would consume the other's wake-up.
HANDLE ThreadWaitEvent() {
    struct Event {
        HANDLE handle = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ~Event() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    thread_local Event event;
    if (event.handle == nullptr) {
        throw std::runtime_error("D3D12: CreateEventExW failed");
    }
    return event.handle;
}

std::atomic<bool> async_recording{false};
std::atomic<bool> partial_submission{false};
std::atomic<bool> continuous_submission{false};

void StoreMax(std::atomic<u64>& target, u64 value) {
    u64 current = target.load(std::memory_order_relaxed);
    while (current < value &&
           !target.compare_exchange_weak(current, value, std::memory_order_release,
                                         std::memory_order_relaxed)) {
    }
}

} // Anonymous namespace

void SetAsyncRecording(bool enabled) {
    async_recording.store(enabled, std::memory_order_relaxed);
}

void SetPartialSubmission(bool enabled) {
    partial_submission.store(enabled, std::memory_order_relaxed);
}

void SetContinuousSubmission(bool enabled) {
    continuous_submission.store(enabled, std::memory_order_relaxed);
}

void PartialSubmitScope::Recorded() {
    recorded_tick = scheduler.CurrentTick();
    scheduler.RecordCompletedOperation();
}

PartialSubmitScope::~PartialSubmitScope() noexcept(false) {
    if (recorded_tick && std::uncaught_exceptions() == 0 &&
        recorded_tick == scheduler.CurrentTick() && scheduler.SubmitWorkIfNeeded()) {
        pending_draws = 0;
    }
}

Scheduler::Scheduler(Device& device_)
    : device{device_}, partial_submission_enabled{partial_submission.load(std::memory_order_relaxed)},
      continuous_submission_enabled{continuous_submission.load(std::memory_order_relaxed)},
      partial_submit{continuous_submission_enabled} {
    ID3D12Device* const dev = device.Get();
    ThrowIfFailed(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                  "CreateFence (scheduler)");
    current_allocator = AcquireAllocator();
    ThrowIfFailed(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         current_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command_list)),
                  "CreateCommandList");
    // Below the GPU thread's priority: it only has to keep up with what the GPU thread records.
    const ListRecorder::Options options{.threaded = async_recording.load(std::memory_order_relaxed),
                                        .profile = VideoCore::Perf::DetailedGpuProfileEnabled(),
                                        .thread_name = "D3D12Recorder",
                                        .priority = Common::ThreadPriority::High};
    recorder.emplace(command_list.Get(), options, [this] { ReleaseWaitersAfterError(); });
    LOG_INFO(Render, "D3D12: command list calls made {}",
             options.threaded ? "on a recording thread" : "on the GPU thread");
    stream.emplace(*recorder);
    CreateTimestamps();
    BeginTimestamp();
    if (partial_submission_enabled && continuous_submission_enabled) {
        LOG_INFO(Render, "D3D12: continuous partial submission, no frame quota; drained queue {} us, queued work {} us, optional submissions defer at {} outstanding lists",
                 PartialSubmitPolicy::DrainedQueueBudgetUs, PartialSubmitPolicy::BudgetUs,
                 PartialSubmitPolicy::MaxInFlight);
    } else {
        LOG_INFO(Render, "D3D12: partial submission {}, budget {} us, minimum {} operations, max {} per frame and {} lists in flight",
                 partial_submission_enabled ? "on" : "off", PartialSubmitPolicy::BudgetUs,
                 PartialSubmitPolicy::MinWork, PartialSubmitPolicy::MaxPerFrame,
                 PartialSubmitPolicy::MaxInFlight);
    }
}

void Scheduler::CreateTimestamps() {
    u64 frequency = 0;
    if (FAILED(device.Queue()->GetTimestampFrequency(&frequency)) || frequency == 0) {
        LOG_WARNING(Render, "D3D12: no queue timestamps; GPU time is not measured");
        return;
    }
    ID3D12Device* const dev = device.Get();
    const D3D12_QUERY_HEAP_DESC heap_desc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                          .Count = TIMESTAMP_SLOTS * 2, .NodeMask = 0};
    const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC desc{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Alignment = 0,
        .Width = TIMESTAMP_SLOTS * 2 * sizeof(u64), .Height = 1, .DepthOrArraySize = 1,
        .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = {.Count = 1, .Quality = 0},
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = D3D12_RESOURCE_FLAG_NONE};
    void* mapped = nullptr;
    if (FAILED(dev->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&timestamp_heap))) ||
        FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&timestamp_readback))) ||
        FAILED(timestamp_readback->Map(0, nullptr, &mapped))) {
        LOG_WARNING(Render, "D3D12: timestamp queries unavailable; GPU time is not measured");
        timestamp_heap.Reset();
        timestamp_readback.Reset();
        return;
    }
    timestamp_data = static_cast<const u64*>(mapped);
    timestamp_us_per_tick = 1'000'000.0 / static_cast<double>(frequency);
}

void Scheduler::BeginTimestamp() {
    timestamp_open = false;
    if (!timestamp_heap) {
        return;
    }
    {
        std::scoped_lock lock{timestamp_mutex};
        if (pending_timestamps.size() >= TIMESTAMP_SLOTS) {
            return; // every slot is still waiting for the GPU: this list goes unmeasured
        }
    }
    timestamp_slot = timestamp_next;
    timestamp_next = (timestamp_next + 1) % TIMESTAMP_SLOTS;
    stream->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamp_slot * 2);
    timestamp_open = true;
}

void Scheduler::EndTimestamp(u64 tick) {
    if (!timestamp_open) {
        return;
    }
    stream->EndQuery(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, timestamp_slot * 2 + 1);
    stream->ResolveQueryData(timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                             timestamp_slot * 2, 2, timestamp_readback.Get(),
                             timestamp_slot * 2 * sizeof(u64));
    std::scoped_lock lock{timestamp_mutex};
    pending_timestamps.emplace_back(tick, timestamp_slot);
}

void Scheduler::ReadTimestamps(u64 gpu_tick) {
    if (!timestamp_data) {
        return;
    }
    std::scoped_lock lock{timestamp_mutex};
    while (!pending_timestamps.empty() && pending_timestamps.front().first <= gpu_tick) {
        const u32 slot = pending_timestamps.front().second;
        pending_timestamps.pop_front();
        const u64 begin = timestamp_data[slot * 2];
        const u64 end = timestamp_data[slot * 2 + 1];
        if (end > begin) {
            VideoCore::Perf::Add(VideoCore::Perf::Counter::GpuBusyUs,
                                 static_cast<u64>(static_cast<double>(end - begin) *
                                                  timestamp_us_per_tick));
        }
    }
}

Scheduler::~Scheduler() {
    // Recorded after the renderer's last drain, while its caches were destroyed: the calls may
    // name objects that are gone, and nothing would submit them.
    recorder->DiscardRecorded();
    try {
        // Teardown runs on whichever thread destroys the renderer; nothing records anymore.
        recording_thread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        DrainForShutdown();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{}", e.what());
    }
    // Before the list and allocators it records into are destroyed.
    stream.reset();
    recorder.reset();
    std::scoped_lock lock{release_mutex};
    pending_releases.clear();
}

void Scheduler::RecordSyncSite(char kind) {
    void* frames[SYNC_SITE_FRAMES + 2]{};
    // Skip this function and its caller inside the scheduler (Flush or Wait).
    const USHORT count = RtlCaptureStackBackTrace(2, static_cast<DWORD>(std::size(frames)),
                                                  frames, nullptr);
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    std::array<uintptr_t, SYNC_SITE_FRAMES> key{};
    size_t used = 0;
    // Several frames, because the direct caller is often Finish or Wait itself.
    for (USHORT i = 0; i < count && used < key.size(); ++i) {
        key[used++] = reinterpret_cast<uintptr_t>(frames[i]) - base;
    }
    std::scoped_lock lock{sync_sites_mutex};
    ++sync_sites[{kind, key}];
}

std::string Scheduler::TakeSyncSites(size_t max_sites) {
    std::vector<std::pair<u64, std::pair<char, std::array<uintptr_t, SYNC_SITE_FRAMES>>>> sorted;
    {
        std::scoped_lock lock{sync_sites_mutex};
        for (const auto& [key, count] : sync_sites) {
            sorted.emplace_back(count, key);
        }
        sync_sites.clear();
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string result;
    for (size_t i = 0; i < std::min(max_sites, sorted.size()); ++i) {
        const auto& [count, key] = sorted[i];
        result += fmt::format("{}{} {}x", i == 0 ? "" : " ; ", key.first, count);
        for (const uintptr_t rva : key.second) {
            if (rva != 0) {
                result += fmt::format(" 0x{:x}", rva);
            }
        }
    }
    return result;
}

u64 Scheduler::Flush() {
    recorder->RethrowError();
    if (const HRESULT reason = device.Get()->GetDeviceRemovedReason(); FAILED(reason)) {
        device.ReportDeviceRemoved();
        throw std::runtime_error(fmt::format("D3D12: flush on removed device 0x{:08X}", static_cast<u32>(reason)));
    }
    if (VideoCore::Perf::DetailedGpuProfileEnabled() && IsRecordingThread()) {
        RecordSyncSite('S');
    }
    std::scoped_lock lock{submit_mutex};
    for (auto& callback : on_submit) {
        callback();
    }
    const u64 signaled = current_tick.load(std::memory_order_relaxed);
    EndTimestamp(signaled);
    if (GpuDiagnosticsEnabled()) {
        TraceGpuDetail(fmt::format("batch RECORD END tick={} completed={} PSO={}", signaled,
                                     fence->GetCompletedValue(), static_cast<void*>(current_pipeline)));
        TraceGpuOperation(fmt::format("submit tick={} completed={} PSO={}", signaled,
                                     fence->GetCompletedValue(), static_cast<void*>(current_pipeline)));
    }
    const auto origin = VideoCore::FrameTrace::CurrentGpuOrigin();
    const bool tracing = VideoCore::FrameTrace::FrameChainActive();
    const SubmissionTrace trace{
        tracing ? VideoCore::FrameTrace::GetCaptureStatus().id : 0,
        origin.work, pending_operations,
        tracing && submission_upload_size ? submission_upload_size() : 0};
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuFlushRecorded,
                               signaled, origin.work);
    stream->Record([this, signaled, trace](ID3D12GraphicsCommandList* list) {
        SubmitRecorded(list, signaled, trace);
    });
    recorder->Dispatch();
    VideoCore::Perf::Add(VideoCore::Perf::Counter::Submits, 1);
    {
        // Publish the flush to threads waiting on it (see Wait).
        std::scoped_lock submitted_lock{submitted_mutex};
        current_tick.store(signaled + 1, std::memory_order_release);
    }
    submitted_cv.notify_all();

    CollectGarbage();
    current_pipeline = nullptr;
    pending_operations = 0;
    BeginTimestamp();
    for (auto& callback : on_reset) {
        callback();
    }
    partial_submit.Submitted();
    return signaled;
}

bool Scheduler::SubmitWorkIfNeeded() {
    if (!partial_submission_enabled || !partial_submit.RecordWork()) return false;
    ++submission_stats.clock_checks;
    const auto now_us = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    if (!partial_submit.Due(now_us)) return false;
    const u64 last_issued = CurrentTick() - 1;
    const u64 submitted = std::min(last_issued, SubmittedTick());
    const u64 completed = KnownGpuTick();
    const u64 outstanding = last_issued > completed ? last_issued - completed : 0;
    if (outstanding >= PartialSubmitPolicy::MaxInFlight) {
        ++submission_stats.backpressure_skips;
        if (last_issued > submitted) ++submission_stats.recorder_backlog_skips;
        if (submitted > completed) ++submission_stats.native_backlog_skips;
        return false;
    }
    if (!partial_submit.Due(now_us, outstanding)) return false;
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuPartialSubmit,
                               CurrentTick(), partial_submit.PendingWork());
    Flush();
    partial_submit.PartiallySubmitted();
    ++submission_stats.partial_submits;
    if (outstanding == 0) ++submission_stats.drained_queue_submits;
    else ++submission_stats.queued_work_submits;
    return true;
}

void Scheduler::BeginFrame() {
    partial_submit.BeginFrame();
    ++submission_frames;
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuFrameBoundary,
                               submission_frames, CurrentTick());
    if (partial_submission_enabled && submission_frames % 300 == 0) {
        LOG_INFO(Render, "D3D12 partial submits: {} submissions, {} clock checks, {} backlog deferrals (cumulative)",
                 submission_stats.partial_submits, submission_stats.clock_checks,
                 submission_stats.backpressure_skips);
        LOG_INFO(Render, "D3D12 partial supply: {} drained-queue deliveries, {} queued-work deliveries, continuous {} (cumulative)",
                 submission_stats.drained_queue_submits, submission_stats.queued_work_submits,
                 continuous_submission_enabled);
        LOG_INFO(Render, "D3D12 partial backlog: {} recorder deferrals, {} native-queue deferrals (overlapping cumulative counts)",
                 submission_stats.recorder_backlog_skips, submission_stats.native_backlog_skips);
    }
}

void Scheduler::SubmitRecorded(ID3D12GraphicsCommandList* list, u64 tick, SubmissionTrace trace) {
    const bool probing = CloseProbeEnabled();
    if (probing) {
        ProbeBeforeClose();
    }
    const bool tracing = trace.capture && VideoCore::FrameTrace::FrameChainActive() &&
                         trace.capture == VideoCore::FrameTrace::GetCaptureStatus().id;
    const auto close_start = tracing ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
    const HRESULT closed = list->Close();
    if (tracing) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuListClose, tick,
                                   VideoCore::Perf::ElapsedUs(close_start));
    }
    if (probing) {
        ProbeReportClose(FAILED(closed));
    }
    if (FAILED(closed)) {
        // An invalid command was recorded; the debug layer (PC) says which before the throw. On
        // the console, dred=1 leaves the commands recorded before it.
        device.LogDebugMessages();
        DumpRecentGpuDetails(4096);
        ThrowIfFailed(closed, "ID3D12GraphicsCommandList::Close");
    }
    ID3D12CommandList* const lists[] = {list};
    {
        // The present thread submits to the same queue (see Device::QueueMutex).
        std::scoped_lock queue_lock{device.QueueMutex()};
        if (tracing) {
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuListPreviousSubmitted,
                                       tick, SubmittedTick());
            VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuListPreviousCompleted,
                                       tick, KnownGpuTick());
        }
        device.Queue()->ExecuteCommandLists(1, lists);
        ThrowIfFailed(device.Queue()->Signal(fence.Get(), tick), "ID3D12CommandQueue::Signal");
    }
    VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuSubmit, tick,
        trace.capture == VideoCore::FrameTrace::GetCaptureStatus().id ? trace.work : 0);
    if (tracing) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuListWork, tick, trace.operations);
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuListUpload, tick, trace.upload_bytes);
    }
    CheckRemovedAfter(device.Get(), [&] {
        return fmt::format("submitting tick {} (removed by the GPU or by a recorded command)",
                           tick);
    });
    submitted_tick.store(tick, std::memory_order_release);
    submitted_tick.notify_all();

    const auto reset_start = tracing ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
    allocator_pool.push_back({std::move(current_allocator), tick});
    current_allocator = AcquireAllocator();
    ThrowIfFailed(list->Reset(current_allocator.Get(), nullptr),
                  "ID3D12GraphicsCommandList::Reset");
    if (tracing) {
        VideoCore::FrameTrace::Mark(VideoCore::FrameTrace::Event::GpuListReset, tick,
                                   VideoCore::Perf::ElapsedUs(reset_start));
    }
    if (CloseProbeEnabled()) {
        ProbeResetState();
    }
}

void Scheduler::ReleaseWaitersAfterError() noexcept {
    // The lists after the failed call are never submitted: complete every tick from the CPU so
    // fence waits return, and the recording thread rethrows at its next Flush or Wait. One below
    // all ones, which KnownGpuTick reads as a removed device.
    fence->Signal(UINT64_MAX - 1);
    submitted_tick.store(UINT64_MAX - 1, std::memory_order_release);
    submitted_tick.notify_all();
}

void Scheduler::WaitSubmitted(u64 tick) {
    for (u64 submitted = SubmittedTick(); submitted < tick; submitted = SubmittedTick()) {
        submitted_tick.wait(submitted, std::memory_order_acquire);
    }
}

void Scheduler::DrainForShutdown() noexcept {
    if (FAILED(device.Get()->GetDeviceRemovedReason())) return;
    try { Finish(); }
    catch (const std::exception& e) { LOG_ERROR(Render, "GPU teardown: {}", e.what()); }
}

void Scheduler::Finish() {
    Wait(Flush());
    CollectGarbage();
}

void Scheduler::Wait(u64 tick) {
    if (tick >= CurrentTick()) {
        if (IsRecordingThread()) {
            Flush();
        } else {
            // Never flush a list another thread is recording into: wait for it to be submitted.
            const VideoCore::FrameTrace::ScopedSpan trace_submit{
                VideoCore::FrameTrace::Event::FenceSubmitWaitLong, tick};
            std::unique_lock lock{submitted_mutex};
            submitted_cv.wait(lock, [this, tick] { return tick < CurrentTick(); });
        }
    }
    if (IsFree(tick)) {
        return;
    }
    // Only the recording (GPU) thread stalls the frame by waiting; the fence thread waits by
    // design.
    const bool counted = IsRecordingThread();
    if (counted && VideoCore::Perf::DetailedGpuProfileEnabled()) {
        RecordSyncSite('W');
    }
    const auto start = std::chrono::steady_clock::now();
    const HANDLE event = ThreadWaitEvent();
    if (GpuDiagnosticsEnabled()) TraceGpuOperation(fmt::format("wait BEGIN tick={}", tick));
    {
        const VideoCore::FrameTrace::ScopedSpan trace_wait{
            VideoCore::FrameTrace::Event::FenceGpuWaitLong, tick};
        ThrowIfFailed(fence->SetEventOnCompletion(tick, event), "ID3D12Fence::SetEventOnCompletion");
        WaitForSingleObjectEx(event, INFINITE, FALSE);
    }
    StoreMax(known_gpu_tick, tick);
    if (counted) {
        // A failed recorded call completes the fence from the CPU (ReleaseWaitersAfterError).
        recorder->RethrowError();
    }
    if (GpuDiagnosticsEnabled()) TraceGpuOperation(fmt::format("wait END tick={} fence={}", tick, fence->GetCompletedValue()));
    if (counted) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::FenceWaits, 1);
        VideoCore::Perf::Add(VideoCore::Perf::Counter::FenceWaitUs,
                             VideoCore::Perf::ElapsedUs(start));
    }
}

u64 Scheduler::KnownGpuTick() const {
    const u64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) {
        // GetCompletedValue returns all ones once the device is removed.
        device.ReportDeviceRemoved();
        throw std::runtime_error(fmt::format("D3D12: device removed (reason 0x{:08X})",
                                             static_cast<u32>(
                                                 device.Get()->GetDeviceRemovedReason())));
    }
    StoreMax(known_gpu_tick, completed);
    return known_gpu_tick.load(std::memory_order_acquire);
}

void Scheduler::DeferRelease(ComPtr<IUnknown> object) {
    if (object) {
        std::scoped_lock lock{release_mutex};
        pending_releases.push_back({CurrentTick(), std::move(object), {}});
    }
}

void Scheduler::DeferRelease(ComPtr<IUnknown> object, std::function<void()>&& retire) {
    if (object) {
        std::scoped_lock lock{release_mutex};
        pending_releases.push_back({CurrentTick(), std::move(object), std::move(retire)});
    } else if (retire) {
        retire();
    }
}

void Scheduler::CollectGarbage() {
    const u64 gpu_tick = KnownGpuTick();
    ReadTimestamps(gpu_tick);
    // Released outside the lock: destroying a resource can take a while, and a retire callback
    // takes its owner's lock, which must not nest inside this one (DeferRelease callers).
    std::vector<PendingRelease> retired;
    {
        std::scoped_lock lock{release_mutex};
        while (!pending_releases.empty() && pending_releases.front().tick <= gpu_tick) {
            retired.push_back(std::move(pending_releases.front()));
            pending_releases.pop_front();
        }
    }
    for (PendingRelease& release : retired) {
        release.object.Reset();
        if (release.retire) {
            release.retire();
        }
    }
}

ComPtr<ID3D12CommandAllocator> Scheduler::AcquireAllocator() {
    // Submission order keeps the pool sorted by tick, so only the front can be free.
    if (!allocator_pool.empty() && IsFree(allocator_pool.front().tick)) {
        ComPtr<ID3D12CommandAllocator> allocator = std::move(allocator_pool.front().allocator);
        allocator_pool.pop_front();
        ThrowIfFailed(allocator->Reset(), "ID3D12CommandAllocator::Reset");
        return allocator;
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    ThrowIfFailed(device.Get()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&allocator)),
                  "CreateCommandAllocator");
    return allocator;
}

} // namespace D3D12
