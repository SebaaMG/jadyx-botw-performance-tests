#define main OriginalUniformGate
#include "incremental-uniform-bindings.cpp"
#undef main
#include <thread>
#include <type_traits>
#include "video_core/renderer_d3d12/d3d12_partial_submit_scope.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"

static_assert(!std::is_nothrow_destructible_v<PartialSubmitScope>);

static void CheckBackpressure(Device& device, bool continuous = false) {
    SetPartialSubmission(true);
    SetContinuousSubmission(continuous);
    Scheduler scheduler{device};
    auto upload = CreateCommittedBuffer(device.Get(), 2048, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "partial input");
    auto readback = CreateCommittedBuffer(device.Get(), 2048, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "partial output");
    void* mapped{};
    const D3D12_RANGE empty{};
    ThrowIfFailed(upload->Map(0, &empty, &mapped), "map partial input");
    for (u32 index = 0; index < 512; ++index) static_cast<u32*>(mapped)[index] = index * 31 + 7;
    upload->Unmap(0, nullptr);
    struct QueueBlock {
        Scheduler& scheduler;
        ComPtr<ID3D12Fence> fence;
        ~QueueBlock() {
            if (fence) fence->Signal(1);
            try { scheduler.Finish(); } catch (...) {}
        }
    } block{scheduler};
    ThrowIfFailed(device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&block.fence)), "block fence");
    ThrowIfFailed(device.Queue()->Wait(block.fence.Get(), 1), "hold partial queue");
    for (u32 index = 0; index < 512; ++index) {
        scheduler.CommandList()->CopyBufferRegion(readback.Get(), index * 4, upload.Get(), index * 4, 4);
        if (index % 128 == 127) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        u32 pending_draws = 1;
        PartialSubmitScope boundary{scheduler, pending_draws};
        boundary.Recorded();
    }
    const auto stats = scheduler.ReadSubmissionStatistics();
    Check(stats.partial_submits == 2, "in-flight limit did not stop additional submissions");
    Check(stats.backpressure_skips > 0, "blocked GPU did not exercise backpressure");
    Check(!scheduler.IsFree(1), "partial submission signalled unfinished work");
    ThrowIfFailed(block.fence->Signal(1), "release partial queue");
    scheduler.Finish();
    const D3D12_RANGE all{0, 2048};
    ThrowIfFailed(readback->Map(0, &all, &mapped), "map partial result");
    for (u32 index = 0; index < 512; ++index)
        Check(static_cast<const u32*>(mapped)[index] == index * 31 + 7, "partial copy lifetime corruption");
    readback->Unmap(0, &empty);
}

static void CheckBoundaries(Device& device, bool continuous = false) {
    SetPartialSubmission(true);
    SetContinuousSubmission(continuous);
    Scheduler scheduler{device};
    u32 pending_draws = 1;
    try {
        PartialSubmitScope boundary{scheduler, pending_draws};
        boundary.Recorded();
        throw std::runtime_error("test unwind");
    } catch (const std::runtime_error&) {}
    Check(scheduler.ReadSubmissionStatistics().clock_checks == 0,
          "unwinding attempted an optional submission");
    {
        PartialSubmitScope boundary{scheduler, pending_draws};
        boundary.Recorded();
        scheduler.Flush();
    }
    Check(scheduler.ReadSubmissionStatistics().clock_checks == 0,
          "already submitted work was counted in the new list");
    scheduler.Finish();
}

static void CheckDraws(Device& device, ID3DBlob* pixel, ID3DBlob* vertex,
                       bool worker, bool partial, u32 pattern, bool continuous = false) {
    SetPartialSubmission(partial);
    SetContinuousSubmission(continuous);
    constexpr u32 Draws = 1024;
    ComPtr<ID3D12QueryHeap> heap;
    const D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 16, 0};
    ThrowIfFailed(device.Get()->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap)), "partial query heap");
    auto query_results = CreateCommittedBuffer(device.Get(), 128, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "partial query results");
    u32 slices{};
    bool query_active{}, resume = true, completed_early{};
    Scheduler::SubmissionStatistics stats;
    const auto result = Run(device, pixel, vertex, false, worker, Draws, 65536, pattern,
        [&](Scheduler& scheduler, u32 index) {
            if (index == 0) {
                scheduler.RegisterOnSubmit([&] {
                    if (!query_active) return;
                    auto* cmd = scheduler.CommandList();
                    cmd->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slices);
                    cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slices, 1,
                                          query_results.Get(), slices * 8);
                    ++slices;
                    query_active = false;
                });
                scheduler.RegisterOnReset([&] {
                    if (resume) {
                        scheduler.CommandList()->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slices);
                        query_active = true;
                    }
                });
                scheduler.CommandList()->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, slices);
                query_active = true;
            }
            if (index + 1 == Draws) resume = false;
            if (index % 128 == 127) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto previous_submits = scheduler.ReadSubmissionStatistics().partial_submits;
            u32 pending_draws = 1;
            {
                PartialSubmitScope boundary{scheduler, pending_draws};
                boundary.Recorded();
            }
            if (scheduler.ReadSubmissionStatistics().partial_submits > previous_submits &&
                (continuous || !completed_early)) {
                const u64 tick = scheduler.CurrentTick() - 1;
                scheduler.WaitSubmitted(tick);
                scheduler.Wait(tick);
                Check((completed_early || index + 1 < Draws) && scheduler.IsFree(tick),
                      "partial work did not finish before the producer");
                Check(!scheduler.IsFree(scheduler.CurrentTick()), "partial completion signalled a future dependency");
                completed_early = true;
            }
            stats = scheduler.ReadSubmissionStatistics();
            if (continuous && (index == 341 || index == 777)) {
                u32 pending = 1;
                InnerFence native_fence{scheduler, false};
                const u64 tick = scheduler.CurrentTick();
                native_fence.Queue([&] { pending = 0; });
                Check(pending == 0 && scheduler.CurrentTick() == tick + 1,
                      "native fence failed to close its query slice and notify the producer");
            }
        });
    Check(partial ? completed_early && stats.partial_submits > 0 : stats.partial_submits == 0,
          "partial submit switch did not change native delivery");
    if (!continuous) Check(stats.partial_submits <= 3, "legacy partial submission exceeded frame cap");
    if (continuous && partial) Check(stats.partial_submits > 3, "continuous supply stopped at the legacy frame quota");
    Check(result.submissions >= stats.partial_submits + 1, "final submission was lost");
    void* mapped{};
    const D3D12_RANGE all{0, slices * 8};
    ThrowIfFailed(query_results->Map(0, &all, &mapped), "read partial queries");
    u64 samples{};
    for (u32 index = 0; index < slices; ++index) samples += static_cast<const u64*>(mapped)[index];
    const D3D12_RANGE empty{};
    query_results->Unmap(0, &empty);
    Check(samples == Draws - 1, "query result changed across partial submissions");
}

static void CheckFenceBoundaries(Device& device, bool coalesce) {
    SetPartialSubmission(false);
    SetContinuousSubmission(false);
    Scheduler scheduler{device};
    StagingBufferPool staging{device, scheduler};
    auto upload = CreateCommittedBuffer(device.Get(), 4096, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "fence input");
    auto readback = CreateCommittedBuffer(device.Get(), 4096, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "fence output");
    auto query_result = CreateCommittedBuffer(device.Get(), 8, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "fence query");
    ComPtr<ID3D12QueryHeap> heap;
    const D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_OCCLUSION, 1, 0};
    ThrowIfFailed(device.Get()->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap)), "fence query heap");
    void* mapped{};
    const D3D12_RANGE empty{};
    ThrowIfFailed(upload->Map(0, &empty, &mapped), "map fence input");
    for (u32 index = 0; index < 1024; ++index) static_cast<u32*>(mapped)[index] = index * 71 + 13;
    upload->Unmap(0, nullptr);
    struct QueueBlock {
        Scheduler& scheduler;
        ComPtr<ID3D12Fence> fence;
        ~QueueBlock() {
            if (fence) fence->Signal(1);
            try { scheduler.Finish(); } catch (...) {}
            scheduler.ClearSubmissionCallbacks();
        }
    } block{scheduler};
    ThrowIfFailed(device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&block.fence)), "fence block");
    ThrowIfFailed(device.Queue()->Wait(block.fence.Get(), 1), "hold fence queue");
    bool query_active = true;
    u32 submit_callbacks{}, reset_callbacks{};
    scheduler.RegisterOnReset([&] { ++reset_callbacks; });
    scheduler.CommandList()->BeginQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
    scheduler.RegisterOnSubmit([&] {
        ++submit_callbacks;
        if (!query_active) return;
        scheduler.CommandList()->EndQuery(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION, 0);
        scheduler.CommandList()->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                                  0, 1, query_result.Get(), 0);
        query_active = false;
    });
    u32 pending = 1, notifications{};
    const std::function<void()> notify = [&] { pending = 0; ++notifications; };
    const u64 first_tick = scheduler.CurrentTick();
    InnerFence stub{scheduler, true};
    stub.Queue(notify);
    Check(scheduler.CurrentTick() == first_tick && pending == 1 && notifications == 0,
          "stub fence submitted or cleared pending work");
    scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, 2048);
    InnerFence first{scheduler, false};
    first.Queue(coalesce ? notify : std::function<void()>{});
    Check(!query_active, "native fence omitted its query resolve");
    if (pending != 0) { scheduler.Flush(); pending = 0; }
    Check(scheduler.CurrentTick() == first_tick + (coalesce ? 1 : 2),
          "fence boundary did not preserve or remove the expected extra submission");
    Check(!first.IsSignaled(), "native fence completed before blocked work");
    scheduler.CommandList()->CopyBufferRegion(readback.Get(), 2048, upload.Get(), 2048, 2048);
    pending = 1;
    const u64 second_tick = scheduler.CurrentTick();
    InnerFence second{scheduler, false};
    second.Queue(coalesce ? notify : std::function<void()>{});
    if (pending != 0) { scheduler.Flush(); pending = 0; }
    Check(scheduler.CurrentTick() == second_tick + (coalesce ? 1 : 2),
          "new work following a native fence was not submitted");
    Check(notifications == (coalesce ? 2u : 0u) && !second.IsSignaled(),
          "producer notification was confused with GPU completion");
    Check(pending == 0 && staging.PendingUploadBytes() == 0,
          "fence boundary retained submitted producer state");
    auto fresh_upload = staging.Request(8ULL << 20, MemoryUsage::Upload, true);
    for (u32 index = 0; index < 16; ++index) {
        const u32 value = index * 71 + 13;
        std::memcpy(fresh_upload.mapped_span.data() + index * sizeof(value), &value, sizeof(value));
    }
    scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, fresh_upload.buffer,
                                              fresh_upload.offset, 64);
    const u64 fresh_tick = scheduler.CurrentTick();
    const u32 prior_callbacks = submit_callbacks;
    Check(staging.PendingUploadBytes() >= (8ULL << 20) && pending == 0,
          "new upload-only work was not visible independently of draws");
    if (pending != 0 || staging.PendingUploadBytes() >= (8ULL << 20)) scheduler.Flush();
    Check(scheduler.CurrentTick() == fresh_tick + 1 && staging.PendingUploadBytes() == 0 &&
          submit_callbacks == prior_callbacks + 1 && reset_callbacks == submit_callbacks,
          "upload-only boundary lost its submission or callbacks");
    ThrowIfFailed(block.fence->Signal(1), "release fence queue");
    first.Wait();
    second.Wait();
    scheduler.Wait(fresh_tick);
    Check(first.IsSignaled() && second.IsSignaled() && !scheduler.IsFree(scheduler.CurrentTick()),
          "native fence signalled the wrong dependency");
    const D3D12_RANGE all{0, 4096};
    ThrowIfFailed(readback->Map(0, &all, &mapped), "map fence output");
    for (u32 index = 0; index < 1024; ++index)
        Check(static_cast<const u32*>(mapped)[index] == index * 71 + 13, "fence readback bytes changed");
    readback->Unmap(0, &empty);
    const D3D12_RANGE query_range{0, 8};
    ThrowIfFailed(query_result->Map(0, &query_range, &mapped), "map fence query");
    Check(*static_cast<const u64*>(mapped) == 0, "fence query resolve was lost");
    query_result->Unmap(0, &empty);
    scheduler.ClearSubmissionCallbacks();
}

static void CheckFenceFailure(Device& device) {
    Scheduler scheduler{device};
    bool notified{};
    scheduler.RegisterOnSubmit([] { throw std::runtime_error("synthetic submit failure"); });
    InnerFence fence{scheduler, false};
    bool failed{};
    try { fence.Queue([&] { notified = true; }); }
    catch (const std::runtime_error&) { failed = true; }
    Check(failed && !notified && scheduler.CurrentTick() == 1,
          "failed native submission cleared pending work");
    scheduler.ClearSubmissionCallbacks();
    scheduler.Finish();
}

#ifndef SERIESX_PARTIAL_SUBMIT_NO_MAIN
int main(int argc, char**) try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    const bool worker = argc > 1;
    SetAsyncRecording(worker);
    Device device;
    ComPtr<ID3D12InfoQueue> debug;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&debug)), "partial info queue");
    debug->ClearStoredMessages();
    const std::string shader =
        "cbuffer A0:register(b0){uint4 a0[32];}; cbuffer A1:register(b1){uint4 a1[32];};"
        "cbuffer A2:register(b2){uint4 a2[32];}; cbuffer A3:register(b3){uint4 a3[32];};"
        "cbuffer A4:register(b4){uint4 a4[32];}; cbuffer A5:register(b5){uint4 a5[32];};"
        "cbuffer A6:register(b6){uint4 a6[32];}; cbuffer A7:register(b7){uint4 a7[32];};"
        "cbuffer Push:register(b0,space" + std::to_string(PUSH_CONSTANT_SPACE) + "){uint index;};"
        "RWBuffer<uint4> result:register(u8); void main(float4 p:SV_Position){"
        "result[index]=a0[0]+a1[0]+a2[0]+a3[0]+a4[0]+a5[0]+a6[0]+a7[0]+a6[16];}";
    const char vertex_shader[] = "float4 main(uint i:SV_VertexID):SV_Position{"
        "return float4(i==1?3:-1,i==2?3:-1,0,1);}";
    ComPtr<ID3DBlob> pixel, vertex, errors;
    ThrowIfFailed(D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, "main", "ps_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors), "partial PS");
    ThrowIfFailed(D3DCompile(vertex_shader, sizeof(vertex_shader), nullptr, nullptr, nullptr, "main", "vs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors), "partial VS");
    CheckBackpressure(device);
    CheckBoundaries(device);
    for (u32 pattern : {0u, 1u, 2u, 3u})
        for (bool partial : {false, true}) CheckDraws(device, pixel.Get(), vertex.Get(), worker, partial, pattern);
    DebugClean(debug.Get());
    std::puts("PASS: early native execution, blocked-queue backpressure, descriptors and query sums exact; D3D12 debug clean");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
#endif
