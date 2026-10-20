#define main OriginalDepthGate
#include "tools/xbox/tests/depth-region-copy.cpp"
#undef main
#include <atomic>
#include <chrono>
#include <thread>
#include "common/host_memory.h"
#include "common/scope_exit.h"
#include "video_core/perf_counters.h"

u64 TestFreeMemory = 46ULL << 20;

bool TestAppMemory(u64& used, u64& limit) {
    limit = 5120ULL << 20;
    used = limit - TestFreeMemory;
    return true;
}

void CheckGuestBacking() {
    constexpr u64 MiB = 1ULL << 20;
    Common::ConfigureHostMemoryFastmem(0, false);
    Common::HostMemory memory{16 * MiB, 0};
    auto* data = memory.BackingBasePointer();
    std::memset(data, 0x5a, 16 * MiB);
    Check(memory.ReleaseBackingRegion(4 * MiB, 2 * MiB) == 2 * MiB,
          "guest free pages not released");
    Check(data[4 * MiB] == 0 && data[6 * MiB - 1] == 0 && data[4 * MiB - 1] == 0x5a &&
              data[6 * MiB] == 0x5a,
          "guest recommit or adjacent data changed");
    std::memset(data, 0, 16 * MiB);
    data[7 * MiB] = 0x37;
    std::size_t cursor{};
    const auto trimmed = memory.Trim(cursor, 16 * MiB);
    Check(trimmed.released >= 14 * MiB && data[7 * MiB] == 0x37 && data[0] == 0,
          "zero page trim changed live data");
    std::atomic_uint64_t published{};
    auto* value = reinterpret_cast<volatile LONG64*>(data + 7 * MiB);
    std::jthread writer([&](std::stop_token stop) {
        u64 sequence = 1;
        while (!stop.stop_requested()) {
            InterlockedExchange64(value, static_cast<LONG64>(sequence));
            published.store(sequence, std::memory_order_release);
            ++sequence;
        }
    });
    for (unsigned iteration = 0; iteration < 20; ++iteration)
        memory.Trim(cursor, MiB);
    writer.request_stop();
    writer.join();
    Check(static_cast<u64>(*value) == published.load(std::memory_order_acquire),
          "concurrent host trim lost a guest write");
    std::cout << "guest backing released " << trimmed.released / 1024
              << " KiB; recommit and concurrent write preserved\n";
}

void CheckReadbackPool(Gate& gate) {
    constexpr u64 MiB = 1ULL << 20;
    constexpr u64 Size = 1050496;
    const CacheMemorySnapshot pressure{(5120 - 46) * MiB, 5120 * MiB, 1700 * MiB, 7367 * MiB};
    SetReadbackReuse(true);
    (void)gate.staging.GuardMemory(pressure);
    std::array<StagingBufferRef, 4> buffers{};
    std::span<u8> source;
    auto upload = CreateMappedBuffer(gate.device.Get(), Size * buffers.size(),
                                     D3D12_HEAP_TYPE_UPLOAD, source);
    for (std::size_t index = 0; index < source.size(); ++index)
        source[index] = u8(index * 37 + index / Size);
    ComPtr<ID3D12Fence> blocker;
    ThrowIfFailed(gate.device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)),
                  "pool blocker");
    SCOPE_EXIT {
        blocker->Signal(1);
    };
    ThrowIfFailed(gate.device.Queue()->Wait(blocker.Get(), 1), "block pool queue");
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        buffers[index] =
            gate.staging.Request(Size, MemoryUsage::Download, true, ReadbackSource::Buffer);
        for (std::size_t previous = 0; previous < index; ++previous)
            Check(buffers[index].buffer != buffers[previous].buffer, "CPU-pinned readbacks alias");
        gate.scheduler.CommandList()->CopyBufferRegion(buffers[index].buffer, 0, upload.Get(),
                                                       index * Size, Size);
        gate.scheduler.Flush();
    }
    ThrowIfFailed(blocker->Signal(1), "unblock pool queue");
    gate.scheduler.Finish();
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        Check(!std::memcmp(buffers[index].mapped_span.data(), source.data() + index * Size, Size),
              "pinned readback corrupted");
        gate.staging.FreeDeferred(buffers[index]);
    }
    gate.scheduler.Finish();
    const auto before = gate.staging.GetReadbackStatistics();
    for (u32 iteration = 0; iteration < 180; ++iteration) {
        auto readback = gate.staging.Request(Size + (iteration % 2) * 256, MemoryUsage::Download,
                                             true, ReadbackSource::Buffer);
        gate.scheduler.CommandList()->CopyBufferRegion(readback.buffer, 0, upload.Get(), 0, Size);
        gate.scheduler.Finish();
        Check(!std::memcmp(readback.mapped_span.data(), source.data(), Size),
              "hot readback changed bytes");
        gate.staging.FreeDeferred(readback);
        gate.scheduler.Finish();
        (void)gate.staging.GuardMemory(pressure);
        gate.staging.TickFrame();
        Check(gate.staging.GetReadbackStatistics().retained_bytes <= ReadbackReserve::Budget,
              "hot reserve grew unbounded");
    }
    const auto after = gate.staging.GetReadbackStatistics();
    Check(after.creates == before.creates, "hot pressure recreated readbacks");
    for (u32 frame = 0; frame < 260; ++frame)
        gate.staging.TickFrame();
    Check(!gate.staging.GetReadbackStatistics().retained_bytes,
          "cold readbacks retained indefinitely");
    gate.CheckDebugLayer("readback retention");
    std::cout << "readback warm loops 180, creations " << after.creates - before.creates
              << ", reuses " << after.reuses - before.reuses << '\n';
}

void CheckSpecificFence(Gate& gate) {
    constexpr u64 MiB = 1ULL << 20;
    const CacheMemorySnapshot pressure{(5120 - 46) * MiB, 5120 * MiB, 1700 * MiB, 7367 * MiB};
    (void)gate.staging.GuardMemory(pressure);
    constexpr u64 Size = 1179648;
    std::span<u8> mapped;
    auto upload = CreateMappedBuffer(gate.device.Get(), Size, D3D12_HEAP_TYPE_UPLOAD, mapped);
    std::memset(mapped.data(), 37, mapped.size());
    auto first = gate.staging.Request(Size, MemoryUsage::Download);
    ComPtr<ID3D12Fence> initial;
    ThrowIfFailed(gate.device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&initial)),
                  "initial blocker");
    SCOPE_EXIT {
        initial->Signal(1);
    };
    ThrowIfFailed(gate.device.Queue()->Wait(initial.Get(), 1), "hold initial submission");
    gate.scheduler.CommandList()->CopyBufferRegion(first.buffer, 0, upload.Get(), 0, Size);
    gate.scheduler.WaitSubmitted(gate.scheduler.Flush());
    ComPtr<ID3D12Fence> unrelated;
    ThrowIfFailed(
        gate.device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&unrelated)),
        "unrelated blocker");
    SCOPE_EXIT {
        unrelated->Signal(1);
    };
    ThrowIfFailed(gate.device.Queue()->Wait(unrelated.Get(), 1), "hold unrelated submission");
    std::span<u8> unrelated_map;
    auto unrelated_buffer =
        CreateMappedBuffer(gate.device.Get(), 64, D3D12_HEAP_TYPE_READBACK, unrelated_map);
    gate.scheduler.CommandList()->CopyBufferRegion(unrelated_buffer.Get(), 0, upload.Get(), 0, 1);
    gate.scheduler.WaitSubmitted(gate.scheduler.Flush());
    std::atomic_bool returned{};
    std::jthread watchdog([&](std::stop_token stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        initial->Signal(1);
        for (unsigned attempt = 0; attempt < 500 && !stop.stop_requested(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!stop.stop_requested())
            unrelated->Signal(1);
    });
    auto reused = gate.staging.Request(Size, MemoryUsage::Download);
    returned = unrelated->GetCompletedValue() == 0;
    watchdog.request_stop();
    ThrowIfFailed(unrelated->Signal(1), "release unrelated submission");
    gate.scheduler.Finish();
    Check(returned && reused.buffer == first.buffer, "request waited on unrelated queue tail");
    gate.CheckDebugLayer("specific readback fence");
}

void CheckAdaptiveReserve(Gate& gate) {
    constexpr u64 MiB = 1ULL << 20;
    const auto measure = [&](u64 free) {
        TestFreeMemory = free;
        (void)gate.staging.GuardMemory({5120 * MiB - free, 5120 * MiB, 1700 * MiB, 7367 * MiB});
    };
    gate.scheduler.Finish();
    gate.staging.ReleaseRetiredDownloads();
    measure(128 * MiB);
    std::array<StagingBufferRef, 3> entries{};
    for (unsigned index = 0; index < entries.size(); ++index) {
        entries[index] = gate.staging.Request(65536ULL << index, MemoryUsage::Download, true);
        gate.staging.FreeDeferred(entries[index]);
        gate.scheduler.Finish();
    }
    for (unsigned index = 0; index < 2; ++index) {
        auto reused = gate.staging.Request(65536ULL << index, MemoryUsage::Download, true);
        gate.staging.FreeDeferred(reused);
        gate.scheduler.Finish();
    }
    measure(MiB);
    const auto trimmed = gate.staging.GetReadbackStatistics();
    Check(trimmed.retention_target == 128 * 1024 &&
              trimmed.retained_bytes <= trimmed.retention_target,
          "completed reserve ignored a lower headroom target");
    auto pinned = gate.staging.Request(128 * 1024, MemoryUsage::Download, true);
    Check(pinned.buffer == entries[1].buffer,
          "reserve evicted the hotter entry before cold allocations");
    measure(0);
    Check(gate.staging.GetReadbackStatistics().retained_bytes == 128 * 1024,
          "target reduction invalidated CPU-pinned storage");
    gate.staging.FreeDeferred(pinned);
    gate.scheduler.Finish();
    measure(0);
    Check(gate.staging.GetReadbackStatistics().retained_bytes == 0,
          "completed ownership kept an exhausted-budget reserve alive");
    measure(46 * MiB);
    gate.CheckDebugLayer("adaptive readback reserve");
    std::cout << "adaptive reserve: cold-first trimming and CPU pin survival passed\n";
}

void CheckCoalescing(Gate& gate) {
    for (auto width : {64U, 65U, 512U}) {
        for (auto height : {1U, 7U, 512U}) {
            auto expected = Pattern(width, height, width + height);
            auto image = gate.Make(PixelFormat::A8B8G8R8_UNORM, expected);
            auto layout = WholeLevel(width, height);
            constexpr u64 Offset = 64;
            std::array<std::vector<u8>, 2> outputs;
            const auto before = VideoCore::Perf::Read();
            for (unsigned mode = 0; mode < outputs.size(); ++mode) {
                SetReadbackCoalescing(mode != 0);
                auto map = gate.staging.Request(layout.buffer_size + Offset + 64,
                                                MemoryUsage::Download, true);
                std::memset(map.mapped_span.data(), 0xcc, map.mapped_span.size());
                image.DownloadMemory(map.buffer, Offset, std::span{&layout, 1});
                gate.scheduler.Finish();
                outputs[mode].assign(map.mapped_span.begin(),
                                     map.mapped_span.begin() + layout.buffer_size + Offset + 64);
                Check(!std::memcmp(outputs[mode].data() + Offset, expected.texels.data(),
                                   layout.buffer_size),
                      "coalesced pixels differ");
                for (u64 index = 0; index < Offset; ++index)
                    Check(outputs[mode][index] == 0xcc, "copy overwrote prefix");
                for (u64 index = Offset + layout.buffer_size; index < outputs[mode].size(); ++index)
                    Check(outputs[mode][index] == 0xcc, "copy overwrote suffix");
                gate.staging.FreeDeferred(map);
            }
            Check(outputs[0] == outputs[1], "coalescing differs from original rows");
            const auto after = VideoCore::Perf::Read();
            if ((width * 4) % 256 == 0)
                Check(VideoCore::Perf::Get(after,
                                           VideoCore::Perf::Counter::TextureReadbackBulkCopies) >
                          VideoCore::Perf::Get(before,
                                               VideoCore::Perf::Counter::TextureReadbackBulkCopies),
                      "contiguous path not exercised");
            gate.CheckDebugLayer("coalesced readback");
        }
    }
    SetReadbackCoalescing(true);
}

void CheckGcBatch(Gate& gate) {
    SetGcReadbackBatching(true);
    (void)gate.runtime.BeginMemoryGuardFrame();
    std::array<std::unique_ptr<Image>, 4> images;
    std::array<Texture, 4> expected;
    std::array<Image*, 4> references{};
    for (unsigned index = 0; index < images.size(); ++index) {
        expected[index] = Pattern(65, 47, index + 3);
        images[index] =
            std::make_unique<Image>(gate.Make(PixelFormat::A8B8G8R8_UNORM, expected[index]));
        references[index] = images[index].get();
        images[index]->flags &= ~ImageFlagBits::CpuModified;
    }
    gate.scheduler.Finish();
    ComPtr<ID3D12Fence> blocker;
    ThrowIfFailed(gate.device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocker)),
                  "GC blocker");
    SCOPE_EXIT {
        blocker->Signal(1);
    };
    ThrowIfFailed(gate.device.Queue()->Wait(blocker.Get(), 1), "block GC queue");
    std::atomic_bool timed_out{};
    std::jthread watchdog([&](std::stop_token stop) {
        for (unsigned attempt = 0; attempt < 1000 && !stop.stop_requested(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!stop.stop_requested()) {
            timed_out = true;
            blocker->Signal(1);
        }
    });
    const auto before = VideoCore::Perf::Read();
    const auto staging_before = gate.staging.GetReadbackStatistics();
    gate.runtime.PrefetchGcDownloads(references);
    for (unsigned index = 0; index < images.size(); ++index) {
        const auto layout = WholeLevel(expected[index].width, expected[index].height);
        StagingBufferRef not_ready{};
        Check(!gate.runtime.PrepareGcDownload(*images[index], std::span{&layout, 1}, not_ready),
              "normal pressure forced synchronous GC");
    }
    const auto submitted = VideoCore::Perf::Read();
    const auto staging_after = gate.staging.GetReadbackStatistics();
    watchdog.request_stop();
    Check(!timed_out, "GC waited on blocked GPU under ordinary memory pressure");
    Check(staging_after.pressure_wait_calls == staging_before.pressure_wait_calls,
          "GC allocation introduced a pressure wait");
    Check(VideoCore::Perf::Get(submitted, VideoCore::Perf::Counter::FenceWaits) ==
              VideoCore::Perf::Get(before, VideoCore::Perf::Counter::FenceWaits),
          "GC synchronized below a legacy headroom threshold");
    Check(VideoCore::Perf::Get(submitted, VideoCore::Perf::Counter::GcReadbackBatches) ==
              VideoCore::Perf::Get(before, VideoCore::Perf::Counter::GcReadbackBatches) + 1,
          "GC batch did not use one grouped submission");
    Check(VideoCore::Perf::Get(submitted, VideoCore::Perf::Counter::GcReadbackBatchImages) ==
              VideoCore::Perf::Get(before, VideoCore::Perf::Counter::GcReadbackBatchImages) + 4,
          "GC batch did not prepare all images");
    ThrowIfFailed(blocker->Signal(1), "unblock GC queue");
    gate.scheduler.Finish();
    for (unsigned index = 0; index < images.size(); ++index) {
        const auto layout = WholeLevel(expected[index].width, expected[index].height);
        StagingBufferRef map{};
        Check(gate.runtime.PrepareGcDownload(*images[index], std::span{&layout, 1}, map),
              "batch readback not ready");
        Check(
            !std::memcmp(map.mapped_span.data(), expected[index].texels.data(), layout.buffer_size),
            "batch data differs");
        gate.runtime.CompleteGcDownload(*images[index]);
    }
    gate.CheckDebugLayer("GC readback batch");
    std::cout << "GC headroom " << TestFreeMemory / (1ULL << 20)
              << " MiB: four images batched, zero pool waits, zero GC waits\n";
}

void CheckGcBudgetAndCancellation(Gate& gate) {
    TestFreeMemory = 48ULL << 20;
    (void)gate.runtime.BeginMemoryGuardFrame();
    std::array<std::unique_ptr<Image>, 5> images;
    std::array<Texture, 5> expected;
    std::array<Image*, 5> references{};
    for (unsigned index = 0; index < images.size(); ++index) {
        expected[index] = Pattern(1024, 512, index + 7);
        images[index] =
            std::make_unique<Image>(gate.Make(PixelFormat::A8B8G8R8_UNORM, expected[index]));
        images[index]->flags &= ~ImageFlagBits::CpuModified;
        references[index] = images[index].get();
    }
    gate.scheduler.Finish();
    const auto before = gate.runtime.GetGcReadbackStatistics();
    gate.runtime.PrefetchGcDownloads(references);
    const auto prepared = gate.runtime.GetGcReadbackStatistics();
    Check(prepared.pending_bytes == (8ULL << 20) && prepared.queued == before.queued + 4,
          "GC pending byte cap ignored");
    ++images.front()->modification_tick;
    const auto layout = WholeLevel(1024, 512);
    StagingBufferRef ignored{};
    Check(!gate.runtime.PrepareGcDownload(*images.front(), std::span{&layout, 1}, ignored),
          "stale GC readback returned as current");
    const auto replaced = gate.runtime.GetGcReadbackStatistics();
    Check(replaced.stale == prepared.stale + 1 && replaced.queued == prepared.queued + 1 &&
              replaced.pending_bytes == prepared.pending_bytes,
          "stale GPU version not replaced");
    images[1]->flags |= ImageFlagBits::CpuModified;
    Check(!gate.runtime.PrepareGcDownload(*images[1], std::span{&layout, 1}, ignored) &&
              gate.runtime.GetGcReadbackStatistics().pending_bytes == (6ULL << 20),
          "CPU modification retained an obsolete GC readback");
    gate.scheduler.Finish();
    for (unsigned index : {0U, 2U, 3U}) {
        StagingBufferRef map{};
        Check(gate.runtime.PrepareGcDownload(*images[index], std::span{&layout, 1}, map),
              "current GC version never became ready");
        Check(
            !std::memcmp(map.mapped_span.data(), expected[index].texels.data(), layout.buffer_size),
            "cancellation changed another pending image");
        gate.runtime.CompleteGcDownload(*images[index]);
    }
    std::array<Image*, 1> remaining{images.back().get()};
    gate.runtime.PrefetchGcDownloads(remaining);
    gate.scheduler.Finish();
    StagingBufferRef map{};
    Check(gate.runtime.PrepareGcDownload(*images.back(), std::span{&layout, 1}, map),
          "cancelled GC ownership leaked the pending budget");
    Check(!std::memcmp(map.mapped_span.data(), expected.back().texels.data(), layout.buffer_size),
          "deferred GC image changed bytes");
    gate.runtime.CompleteGcDownload(*images.back());
    gate.CheckDebugLayer("GC budget and cancellation");
    std::cout << "GC pending cap 8 MiB: oversized batch deferred; stale and CPU-modified copies "
                 "discarded\n";
}

int main(int argc, char** argv) try {
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    SetAsyncRecording(argc > 1 && std::string{argv[1]} == "--async");
    CheckGuestBacking();
    SetAppMemoryQuery(TestAppMemory);
    Gate gate;
    CheckReadbackPool(gate);
    CheckSpecificFence(gate);
    CheckAdaptiveReserve(gate);
    CheckCoalescing(gate);
    for (const auto headroom : {48U, 64U, 80U, 128U}) {
        TestFreeMemory = u64{headroom} << 20;
        CheckGcBatch(gate);
    }
    CheckGcBudgetAndCancellation(gate);
    std::cout << "PASS: readback ownership, pressure reuse, contiguous copies and GC batches; "
                 "D3D12 debug clean\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
