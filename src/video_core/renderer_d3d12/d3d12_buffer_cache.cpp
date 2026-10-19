// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/logging.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_draw_metrics.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/surface.h"

namespace D3D12 {
namespace {
std::atomic_bool uniform_upload_copy_enabled{};
std::atomic_bool coherent_uniform_cache_enabled{};
}
void SetCoherentUniformCache(bool enabled) {
    coherent_uniform_cache_enabled.store(enabled, std::memory_order_relaxed);
}
void SetUniformUploadCopy(bool enabled) {
    uniform_upload_copy_enabled.store(enabled, std::memory_order_relaxed);
}


void BufferCacheRuntime::ReadCheckpointBuffer(Buffer& buffer, u64 offset,
                                             std::span<u8> output) {
    constexpr u64 Limit = 1024 * 1024;
    if (output.empty() || output.size() > Limit || offset > buffer.SizeBytes() ||
        output.size() > buffer.SizeBytes() - offset) {
        throw std::invalid_argument("Pulse: invalid buffer readback range");
    }
    std::span<u8> mapped;
    // Operation-local allocation: unlike Request(), this never becomes a resident pool entry.
    auto readback = CreateMappedBuffer(device.Get(), output.size(), D3D12_HEAP_TYPE_READBACK,
                                       mapped);
    auto charge = std::make_shared<Common::MemoryCharge>(Common::MemoryAccount::StagingReadback,
                                                       Common::AlignUp<u64>(output.size(), 65536));
    // Retain before recording the first reference. If recording/Finish fails, the scheduler owns
    // the resource until its tick retires (or device teardown), rather than leaving a dangling
    // raw pointer in an unsubmitted command stream.
    scheduler.DeferRelease(readback, [charge] {});
    buffer.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, buffer.Handle(), offset,
                                               output.size());
    scheduler.Finish();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "Pulse buffer readback device");
    const D3D12_RANGE range{0, output.size()};
    void* data{};
    ThrowIfFailed(readback->Map(0, &range, &data), "Map Pulse buffer readback");
    std::memcpy(output.data(), data, output.size());
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
}

namespace {

using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

/// Rewrites count vertices of an emulated topology as a list D3D12 can draw; vertex(i) gives the
/// vertex number of the i-th guest vertex. Same triangulation as the Vulkan backend's quad passes.
template <typename Vertex>
void AssembleTopology(PrimitiveTopology topology, u32 count, Vertex&& vertex,
                      std::vector<u32>& out) {
    out.clear();
    switch (topology) {
    case PrimitiveTopology::Quads:
        for (u32 quad = 0; quad + 4 <= count; quad += 4) {
            for (const u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) {
                out.push_back(vertex(quad + i));
            }
        }
        break;
    case PrimitiveTopology::QuadStrip:
        for (u32 base = 0; base + 4 <= count; base += 2) {
            for (const u32 i : {0u, 3u, 1u, 0u, 2u, 3u}) {
                out.push_back(vertex(base + i));
            }
        }
        break;
    case PrimitiveTopology::TriangleFan:
    case PrimitiveTopology::Polygon:
        for (u32 i = 1; i + 1 < count; ++i) {
            out.push_back(vertex(0));
            out.push_back(vertex(i));
            out.push_back(vertex(i + 1));
        }
        break;
    case PrimitiveTopology::LineLoop:
        if (count < 2) {
            break;
        }
        for (u32 i = 0; i + 1 < count; ++i) {
            out.push_back(vertex(i));
            out.push_back(vertex(i + 1));
        }
        out.push_back(vertex(count - 1));
        out.push_back(vertex(0));
        break;
    default:
        for (u32 i = 0; i < count; ++i) {
            out.push_back(vertex(i));
        }
        break;
    }
}

u32 IndexSize(IndexFormat format) {
    switch (format) {
    case IndexFormat::UnsignedByte:
        return 1;
    case IndexFormat::UnsignedShort:
        return 2;
    case IndexFormat::UnsignedInt:
        return 4;
    }
    return 4;
}

} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams params)
    : VideoCommon::BufferBase(params), scheduler{&runtime.scheduler},
      id{runtime.next_buffer_id++}, buffer{runtime.CreateDefaultBuffer(4, allocation)}, address{buffer ? buffer->GetGPUVirtualAddress() : 0},
      tracker{4096}, charge{Common::MemoryAccount::BufferCache, 4} {}

Buffer::Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr, u64 size_bytes, bool)
    : VideoCommon::BufferBase(cpu_addr, size_bytes), scheduler{&runtime.scheduler},
      id{runtime.next_buffer_id++},
      // Whole 256-byte blocks: a guest cbuf ending with the buffer still fits a CBV.
      buffer{runtime.CreateDefaultBuffer(
          Common::AlignUp(size_bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), allocation)},
      address{buffer ? buffer->GetGPUVirtualAddress() : 0}, tracker{size_bytes},
      charge{Common::MemoryAccount::BufferCache,
             Common::AlignUp(size_bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)} {}

Buffer::~Buffer() {
    if (scheduler && buffer) {
        TextureResourceAllocator::DeferRelease(*scheduler, {std::move(buffer), std::move(allocation)});
    }
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (scheduler && buffer) {
            TextureResourceAllocator::DeferRelease(*scheduler, {std::move(buffer), std::move(allocation)});
        }
        static_cast<VideoCommon::BufferBase&>(*this) = std::move(other);
        scheduler = other.scheduler;
        id = other.id;
        generation = other.generation;
        allocation = std::move(other.allocation);
        buffer = std::move(other.buffer);
        address = std::exchange(other.address, 0);
        tracker = std::move(other.tracker);
        state = other.state;
        state_tick = other.state_tick;
        charge = std::move(other.charge);
    }
    return *this;
}

void Buffer::Transition(D3D12_RESOURCE_STATES next) {
    if (!scheduler || !buffer) {
        return;
    }
    if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS || next == D3D12_RESOURCE_STATE_COPY_DEST) {
        ++generation;
    }
    const u64 tick = scheduler->CurrentTick();
    const D3D12_RESOURCE_STATES current = state_tick == tick ? state : D3D12_RESOURCE_STATE_COMMON;
    CommandStream* const cmd = scheduler->CommandList();
    if (current == next) {
        if (next == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            // Orders the previous draw's or dispatch's writes before this one.
            const D3D12_RESOURCE_BARRIER barrier = UavBarrier(buffer.Get());
            cmd->ResourceBarrier(1, &barrier);
        } else if (next == D3D12_RESOURCE_STATE_COPY_DEST) {
            // Copies into one resource are unordered without a barrier between them, and the
            // Series runs them in parallel: CreateBuffer's zero fill landed over the data
            // JoinOverlap copies in. A round trip through COMMON waits for the earlier copy.
            const D3D12_RESOURCE_BARRIER barriers[2]{
                TransitionBarrier(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_COMMON),
                TransitionBarrier(buffer.Get(), D3D12_RESOURCE_STATE_COMMON,
                                  D3D12_RESOURCE_STATE_COPY_DEST),
            };
            cmd->ResourceBarrier(2, barriers);
        }
    } else {
        TransitionResource(cmd, buffer.Get(), current, next);
    }
    state = next;
    state_tick = tick;
}

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                       StagingBufferPool& staging_,
                                       TransferBufferPool& transfer_buffers_,
                                       Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : device{device_}, scheduler{scheduler_}, staging{staging_},
      transfer_buffers{transfer_buffers_}, device_memory{device_memory_},
      buffer_allocator{device_, scheduler_, true},
      uniform_upload_copy{uniform_upload_copy_enabled.load(std::memory_order_relaxed)},
      coherent_uniform_cache{coherent_uniform_cache_enabled.load(std::memory_order_relaxed)} {
    LOG_INFO(Render, "D3D12: buffer cache runtime ready (short uniform upload copy {})", uniform_upload_copy ? "on" : "off");

}

BufferCacheRuntime::~BufferCacheRuntime() {
    RetirePersistentUploads();
    if (zero_buffer) scheduler.DeferRelease(std::move(zero_buffer));
}

ComPtr<ID3D12Resource> BufferCacheRuntime::CreateDefaultBuffer(
    u64 size, std::shared_ptr<TextureResourceAllocator::Allocation>& allocation) {
    if (PersistentResourcesSupported(device.Get())) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto resource = buffer_allocator.Create(desc, D3D12_RESOURCE_STATE_COMMON);
        if (resource.placed) {
            constexpr u64 ZERO_SIZE = 4 * 1024 * 1024;
            if (!zero_buffer) {
                std::span<u8> mapped;
                zero_buffer = CreateMappedBuffer(device.Get(), ZERO_SIZE, D3D12_HEAP_TYPE_UPLOAD, mapped);
                std::memset(mapped.data(), 0, mapped.size());
            }
            auto* cmd = scheduler.CommandList();
            const D3D12_RESOURCE_BARRIER alias{
                .Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING,
                .Aliasing = {.pResourceBefore = nullptr, .pResourceAfter = resource.resource.Get()}};
            cmd->ResourceBarrier(1, &alias);
            TransitionResource(cmd, resource.resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                               D3D12_RESOURCE_STATE_COPY_DEST);
            for (u64 offset = 0; offset < size; offset += ZERO_SIZE)
                cmd->CopyBufferRegion(resource.resource.Get(), offset, zero_buffer.Get(), 0,
                                      std::min(ZERO_SIZE, size - offset));
            TransitionResource(cmd, resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_COMMON);
        }
        allocation = std::move(resource.allocation);
        return std::move(resource.resource);
    }
    // Storage buffers and image buffers are UAVs.
    HRESULT hr = S_OK;
    const auto create = [&] {
        return CreateCommittedBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     "CreateCommittedResource (buffer cache)", &hr);
    };
    ComPtr<ID3D12Resource> buffer = create();
    if (!buffer && hr == E_OUTOFMEMORY &&
        transfer_buffers.ReclaimAfterOutOfMemory(size, "cached buffer")) {
        buffer = create();
        if (buffer) {
            LOG_INFO(Render, "D3D12: cached buffer created after reclaiming");
        }
    }
    if (!buffer) {
        ThrowIfFailed(hr, "CreateCommittedResource (buffer cache)");
    }
    return buffer;
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& buffers) noexcept {
    RetirePersistentUploads();
    ++stats_frame;
    if (coherent_uniform_cache && stats_frame % 300 == 0) {
        const auto stats = uniform_cache_policy.GetStatistics();
        LOG_INFO(Render, "SeriesX coherent uniforms: clean {}, streamed {}, probes {}, "
                 "backoffs {} (cumulative)", stats.clean, stats.streamed, stats.probes, stats.backoffs);
    }
    if (PersistentResourcesSupported(device.Get())) {
        buffer_allocator.TrimEmptyHeaps(false);
        if (stats_frame % 300 == 0 && uniform_content) {
            const auto stats = uniform_content->GetStats();
            LOG_INFO(Render, "SeriesX persistent uniforms: hits={} misses={} saved_bytes={} heaps={}",
                     stats.hits, stats.misses, stats.saved_bytes, buffer_allocator.Report());
            if (descriptor_queue) {
                const auto descriptors = descriptor_queue->GetPersistentStats();
                LOG_INFO(Render, "SeriesX persistent descriptors: hits={} misses={} evictions={} waits={}",
                         descriptors.hits, descriptors.misses, descriptors.evictions, descriptors.waits);
            }
        }
    }
    for (auto it = buffers.begin(); it != buffers.end(); ++it) {
        it->ResetUsageTracking();
    }
}
void BufferCacheRuntime::Finish() { scheduler.Finish(); }
u64 BufferCacheRuntime::CurrentTick() { return scheduler.CurrentTick(); }
bool BufferCacheRuntime::IsFree(u64 tick) { return scheduler.IsFree(tick); }
void BufferCacheRuntime::Wait(u64 tick) { scheduler.Wait(tick); }
u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    const u64 budget = device.CacheMemoryBudget();
    return budget != 0 ? budget : 4ULL * 1024 * 1024 * 1024;
}
u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.CacheMemoryUsage();
}
StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) { return staging.Request(size, MemoryUsage::Upload); }
StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) { return staging.Request(size, MemoryUsage::Download, deferred); }
void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) { staging.FreeDeferred(ref); }

void BufferCacheRuntime::Copy(Buffer* dst, ID3D12Resource* dst_raw, Buffer* src,
                              ID3D12Resource* src_raw,
                              std::span<const VideoCommon::BufferCopy> copies) {
    auto* cmd = scheduler.CommandList();
    if (dst_raw == src_raw) {
        // A buffer cannot be COPY_SOURCE and COPY_DEST at once: bounce through a scratch buffer.
        u64 scratch_size = 0;
        for (const auto& copy : copies) {
            scratch_size += copy.size;
        }
        if (scratch_size == 0) {
            return;
        }
        // In COMMON, new or reused: the copy below promotes it to COPY_DEST.
        ComPtr<ID3D12Resource> scratch = transfer_buffers.Acquire(scratch_size);
        std::vector<VideoCommon::BufferCopy> to_scratch;
        std::vector<VideoCommon::BufferCopy> from_scratch;
        u64 offset = 0;
        for (const auto& copy : copies) {
            to_scratch.push_back({.src_offset = copy.src_offset, .dst_offset = offset,
                                  .size = copy.size});
            from_scratch.push_back({.src_offset = offset, .dst_offset = copy.dst_offset,
                                    .size = copy.size});
            offset += copy.size;
        }
        Copy(nullptr, scratch.Get(), src, src_raw, to_scratch);
        const D3D12_RESOURCE_BARRIER to_source{
            .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Transition = {.pResource = scratch.Get(),
                           .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           .StateBefore = D3D12_RESOURCE_STATE_COPY_DEST,
                           .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE}};
        cmd->ResourceBarrier(1, &to_source);
        Copy(dst, dst_raw, nullptr, scratch.Get(), from_scratch);
        transfer_buffers.Release(std::move(scratch));
        return;
    }
    // Cache buffers are tracked (Buffer::Transition). Raw resources are staging memory, whose
    // state never changes, or the scratch buffer above, promoted from COMMON.
    const bool restore_read = dst && dst->State() == D3D12_RESOURCE_STATE_GENERIC_READ;
    if (dst) dst->Transition(D3D12_RESOURCE_STATE_COPY_DEST);
    if (src) src->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    for (const auto& copy : copies) {
        cmd->CopyBufferRegion(dst_raw, copy.dst_offset, src_raw, copy.src_offset, copy.size);
        if (dst) dst->MarkUsage(copy.dst_offset, copy.size);
        if (src) src->MarkUsage(copy.src_offset, copy.size);
    }
    // An upload can touch a vertex/index buffer whose IA binding is still cached. Restore its
    // readable state: the generic cache may skip rebinding an unchanged guest vertex stream.
    if (restore_read) dst->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
}
void BufferCacheRuntime::CopyBuffer(Buffer& dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies, bool, bool) { Copy(&dst, dst.Handle(), &src, src.Handle(), copies); }
void BufferCacheRuntime::CopyBuffer(Buffer& dst, ID3D12Resource* src, std::span<const VideoCommon::BufferCopy> copies, bool, bool) { Copy(&dst, dst.Handle(), nullptr, src, copies); }
void BufferCacheRuntime::CopyBuffer(ID3D12Resource* dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies, bool, bool) { Copy(nullptr, dst, &src, src.Handle(), copies); }

void BufferCacheRuntime::ClearBuffer(Buffer& dst, u32 offset, size_t size, u32 value) {
    auto upload = staging.Request(size, MemoryUsage::Upload);
    // Zero, the usual value, in one memset; others word by word, then the partial last word.
    const size_t words = size / sizeof(value);
    u8* const data = upload.mapped_span.data();
    if (value == 0) {
        std::memset(data, 0, size);
    } else {
        for (size_t word = 0; word < words; ++word) {
            std::memcpy(data + word * sizeof(value), &value, sizeof(value));
        }
        std::memcpy(data + words * sizeof(value), &value, size - words * sizeof(value));
    }
    const VideoCommon::BufferCopy copy{.src_offset = upload.offset, .dst_offset = offset, .size = size};
    CopyBuffer(dst, upload.buffer, {&copy, 1}, true);
}

bool BufferCacheRuntime::IsEmulatedTopology(PrimitiveTopology topology) noexcept {
    switch (topology) {
    case PrimitiveTopology::Quads:
    case PrimitiveTopology::QuadStrip:
    case PrimitiveTopology::TriangleFan:
    case PrimitiveTopology::Polygon:
    case PrimitiveTopology::LineLoop:
        return true;
    default:
        return false;
    }
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat format,
                                         u32 first, u32 num_indices, Buffer& buffer, u32 offset,
                                         u32 size) {
    rewritten_count.reset();
    if (trace_buffers) {
        const bool rewritten = IsEmulatedTopology(topology) || format == IndexFormat::UnsignedByte;
        trace_buffers->push_back({.kind = rewritten ? TracedBuffer::Kind::RewrittenIndex
                                                    : TracedBuffer::Kind::Index,
                                  .buffer = &buffer, .offset = offset, .size = size,
                                  .stride = IndexSize(format),
                                  .device_addr = buffer.CpuAddr() + offset});
    }
    if (!IsEmulatedTopology(topology) && format != IndexFormat::UnsignedByte) {
        buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
        pending_index = D3D12_INDEX_BUFFER_VIEW{
            .BufferLocation = buffer.Address() + offset,
            .SizeInBytes = size,
            .Format = format == IndexFormat::UnsignedShort ? DXGI_FORMAT_R16_UINT
                                                           : DXGI_FORMAT_R32_UINT,
        };
        return;
    }
    // D3D12 has neither these topologies nor 8-bit indices: rewrite the guest's indices on the
    // CPU from guest memory (the Vulkan backend uses compute passes instead), remembering the
    // rewrite: most draws repeat every frame with the same indices.
    // TODO: inline index draws (inline_index_draw_indexes) never reach guest memory.
    if (!logged_rewrite) {
        LOG_INFO(Render, "D3D12: rewriting indexed draws on the CPU (topology {}, format {})",
                 static_cast<u32>(topology), static_cast<u32>(format));
        logged_rewrite = true;
    }
    const u32 index_size = IndexSize(format);
    const u64 byte_offset = offset + static_cast<u64>(first) * index_size;
    const IndexRewriteCache::Key key{.buffer_id = buffer.Id(), .offset = byte_offset,
                                     .count = num_indices, .format = static_cast<u8>(index_size),
                                     .topology = static_cast<u8>(topology)};
    IndexRewriteCache::Entry* entry = rewrite_cache.Find(key);
    if (entry && entry->generation == buffer.Generation()) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::IndexRewriteReuses, 1);
        BindRewrittenIndices(*entry);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    guest_indices.resize(static_cast<size_t>(num_indices) * index_size);
    device_memory.ReadBlockUnsafe(buffer.CpuAddr() + byte_offset, guest_indices.data(),
                                  guest_indices.size());
    if (entry && entry->source == guest_indices) {
        // Another part of the buffer was written, not these indices.
        entry->generation = buffer.Generation();
        VideoCore::Perf::Add(VideoCore::Perf::Counter::IndexRewriteRechecks, 1);
    } else {
        const auto read = [&](u32 i) -> u32 {
            const u8* const data = guest_indices.data() + static_cast<size_t>(i) * index_size;
            switch (index_size) {
            case 1:
                return data[0];
            case 2: {
                u16 value;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }
            default: {
                u32 value;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }
            }
        };
        AssembleTopology(topology, num_indices, read, rewrite_scratch);
        entry = &rewrite_cache.Obtain(key);
        entry->generation = buffer.Generation();
        entry->source.assign(guest_indices.begin(), guest_indices.end());
        entry->SetRewrite(rewrite_scratch, format == IndexFormat::UnsignedInt);
        rewrite_cache.Account();
        VideoCore::Perf::Add(VideoCore::Perf::Counter::IndexRewrites, 1);
    }
    VideoCore::Perf::Add(VideoCore::Perf::Counter::IndexRewriteNs,
                         static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count()));
    BindRewrittenIndices(*entry);
}

void BufferCacheRuntime::EmulateTopology(PrimitiveTopology topology, u32 first, u32 count) {
    // A generated list depends on nothing but the draw: no generation, no source bytes.
    const IndexRewriteCache::Key key{.buffer_id = 0, .offset = first, .count = count,
                                     .format = 0, .topology = static_cast<u8>(topology)};
    IndexRewriteCache::Entry* entry = rewrite_cache.Find(key);
    if (entry) {
        VideoCore::Perf::Add(VideoCore::Perf::Counter::IndexRewriteReuses, 1);
    } else {
        AssembleTopology(topology, count, [first](u32 i) { return first + i; }, rewrite_scratch);
        entry = &rewrite_cache.Obtain(key);
        entry->SetRewrite(rewrite_scratch, first + count > 0xFFFF);
        rewrite_cache.Account();
        VideoCore::Perf::Add(VideoCore::Perf::Counter::IndexRewrites, 1);
    }
    BindRewrittenIndices(*entry);
}

void BufferCacheRuntime::BindRewrittenIndices(IndexRewriteCache::Entry& entry) {
    rewritten_count = entry.count;
    if (entry.count == 0) {
        return;
    }
    const u64 tick = scheduler.CurrentTick();
    if (entry.upload_tick != tick) {
        // Staging memory of an earlier list may be recycled by now. Within one list, every draw
        // of these indices shares one upload.
        const StagingBufferRef upload = staging.Request(entry.rewrite.size(), MemoryUsage::Upload);
        std::memcpy(upload.mapped_span.data(), entry.rewrite.data(), entry.rewrite.size());
        entry.upload_tick = tick;
        entry.upload_address = upload.buffer->GetGPUVirtualAddress() + upload.offset;
    }
    pending_index = D3D12_INDEX_BUFFER_VIEW{
        .BufferLocation = entry.upload_address,
        .SizeInBytes = static_cast<UINT>(entry.rewrite.size()),
        .Format = entry.wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
    };
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size,
                                          u32 stride) {
    if (trace_buffers) {
        trace_buffers->push_back({.kind = TracedBuffer::Kind::Vertex, .slot = index,
                                  .buffer = &buffer, .offset = offset, .size = size,
                                  .stride = stride, .device_addr = buffer.CpuAddr() + offset});
    }
    buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    BindVertexBuffer(index, buffer.Handle(), offset, size, stride);
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, ID3D12Resource* buffer, u32 offset,
                                          u32 size, u32 stride) {
    if (index >= VideoCommon::NUM_VERTEX_BUFFERS || !buffer) {
        return;
    }
    pending_vertex[index] = D3D12_VERTEX_BUFFER_VIEW{
        .BufferLocation = buffer->GetGPUVirtualAddress() + offset,
        .SizeInBytes = size,
        .StrideInBytes = stride,
    };
    pending_vertex_mask |= 1u << index;
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    for (u32 i = bindings.min_index; i < bindings.max_index; ++i) {
        const u32 slot = i - bindings.min_index;
        BindVertexBuffer(i, *bindings.buffers[slot], static_cast<u32>(bindings.offsets[slot]),
                         static_cast<u32>(bindings.sizes[slot]),
                         static_cast<u32>(bindings.strides[slot]));
    }
}

void BufferCacheRuntime::ApplyGeometry(CommandStream* cmd) {
    if (pending_index && (!applied_index || std::memcmp(&*applied_index, &*pending_index,
                                                        sizeof(D3D12_INDEX_BUFFER_VIEW)) != 0)) {
        cmd->IASetIndexBuffer(&*pending_index);
        applied_index = pending_index;
    }
    // One call per run of consecutive slots (usually a single run from slot 0).
    for (u32 mask = pending_vertex_mask; mask != 0;) {
        const u32 first = static_cast<u32>(std::countr_zero(mask));
        const u32 count = static_cast<u32>(std::countr_one(mask >> first));
        cmd->IASetVertexBuffers(first, count, &pending_vertex[first]);
        mask &= count + first >= 32 ? 0u : ~0u << (first + count);
    }
    pending_index.reset();
    pending_vertex_mask = 0;
    rewritten_count.reset();
}

std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t, u32, u32 size) {
    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvStreamed, 1);
    if (VideoCore::Perf::DetailedGpuProfileEnabled()) {
        DrawMetrics::uniform_upload_bytes.fetch_add(size, std::memory_order_relaxed);
        const size_t bin = size <= 64 ? 0 : size <= 128 ? 1 : size <= 256 ? 2 : size <= 512 ? 3 : 4;
        DrawMetrics::uniform_upload_sizes[bin].fetch_add(1, std::memory_order_relaxed);
    }
    const UniformUpload upload = uniform_stream.Request(size, scheduler.CurrentTick(),
        [this](size_t bytes) {
            const auto ref = staging.Request(bytes, MemoryUsage::Upload);
            return UniformUpload{ref.mapped_span, ref.buffer->GetGPUVirtualAddress() + ref.offset, ref.identity};
        });
    if (trace_buffers) {
        trace_buffers->push_back({.kind = TracedBuffer::Kind::StreamedUniform,
                                  .slot = traced_uniforms++, .size = size,
                                  .mapped = upload.mapped.data()});
    }
    if (descriptor_queue) {
        descriptor_queue->AddConstantBuffer(upload.address, size, upload.identity);
    }
    return upload.mapped.first(size);
}

void BufferCacheRuntime::BindUniformBuffer(Buffer& buffer, u32 offset, u32 size) {
    if (!descriptor_queue) {
        return;
    }
    // A CBV spans whole 256-byte blocks and must stay inside its resource; the null buffer (an
    // unbound guest cbuf) does not hold one.
    const u64 aligned = Common::AlignUp(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    const bool fits = buffer.Handle() && buffer.SizeBytes() != 0 &&
                      offset + aligned <= Common::AlignUp(buffer.SizeBytes(),
                                                          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (trace_buffers) {
        trace_buffers->push_back({.kind = fits ? TracedBuffer::Kind::Uniform
                                               : TracedBuffer::Kind::NullUniform,
                                  .slot = traced_uniforms++, .buffer = fits ? &buffer : nullptr,
                                  .offset = offset, .size = size,
                                  .stride = static_cast<u32>(buffer.SizeBytes()),
                                  .device_addr = buffer.CpuAddr() + offset});
    }
    if (!fits) {
        VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvNull, 1);
        descriptor_queue->AddConstantBuffer(0, 0);
        return;
    }
    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvPersistent, 1);
    buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    descriptor_queue->AddConstantBuffer(buffer.Address() + offset, size, (1ull << 62) | buffer.Id());
}

void BufferCacheRuntime::BindStorageBuffer(Buffer& buffer, u32 offset, u32 size,
                                           bool is_written) {
    if (!descriptor_queue) {
        return;
    }
    // A buffer the cache merged with other data (vertices, say) stays in the state of its last
    // binding; AMD hardware, the Series included, reads buffers in any state.
    buffer.Transition(is_written ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                 : D3D12_RESOURCE_STATE_GENERIC_READ);
    const u64 available = buffer.SizeBytes() > offset ? buffer.SizeBytes() - offset : 0;
    const u32 clamped = static_cast<u32>(std::min<u64>(size, available));
    descriptor_queue->AddStorageBuffer(clamped != 0 ? buffer.Handle() : nullptr, offset, clamped, (1ull << 62) | buffer.Id());
}

void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    if (!descriptor_queue) {
        return;
    }
    buffer.Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
    descriptor_queue->AddTexelBuffer(buffer.Handle(), offset, size, SurfaceFormat(format).view,
                                     VideoCore::Surface::BytesPerBlock(format), false, (1ull << 62) | buffer.Id());
}

void BufferCacheRuntime::BindImageBuffer(Buffer& buffer, u32 offset, u32 size,
                                         VideoCore::Surface::PixelFormat format) {
    if (!descriptor_queue) {
        return;
    }
    buffer.Transition(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    descriptor_queue->AddTexelBuffer(buffer.Handle(), offset, size, SurfaceFormat(format).view,
                                     VideoCore::Surface::BytesPerBlock(format), true, (1ull << 62) | buffer.Id());
}

UniformUpload BufferCacheRuntime::UploadPersistentBytes(std::span<const u8> bytes) {
    if (!uniform_content) uniform_content = std::make_unique<UniformContentCache>();
    return uniform_content->Upload(bytes, scheduler.CurrentTick(), [this](size_t size) {
        constexpr size_t PAGE_SIZE = 65536;
        const size_t aligned = Common::AlignUp(size, size_t{256});
        const u64 tick = scheduler.CurrentTick();
        if (persistent_block_tick != tick || persistent_block.mapped.empty() ||
            aligned > persistent_block.mapped.size() - persistent_block_used) {
            const auto ref = staging.Request(std::max(PAGE_SIZE, aligned), MemoryUsage::Upload, true);
            persistent_uploads.push_back(ref);
            persistent_block = {ref.mapped_span, ref.buffer->GetGPUVirtualAddress() + ref.offset, ref.identity};
            persistent_block_used = 0;
            persistent_block_tick = tick;
        }
        const UniformUpload upload{persistent_block.mapped.subspan(persistent_block_used, size),
            persistent_block.address + persistent_block_used, persistent_block.identity};
        persistent_block_used += aligned;
        return upload;
    });
}

void BufferCacheRuntime::RetirePersistentUploads() {
    if (uniform_content) uniform_content->Invalidate();
    persistent_block = {};
    persistent_block_used = 0;
    // FreeDeferred assigns the CURRENT tick, which covers the draws actually recorded after
    // any helper flushes. The staging pool still waits for that tick's GPU fence before reuse.
    for (auto& ref : persistent_uploads) staging.FreeDeferred(ref);
    persistent_uploads.clear();
}

bool BufferCacheRuntime::BindPersistentUniformBuffer(DAddr address, u32 size) {
    if (!descriptor_queue || !descriptor_queue->IsPersistent() || trace_buffers ||
        size == 0 || size > UniformContentCache::MAX_SIZE) return false;
    device_memory.ReadBlockUnsafe(address, uniform_scratch.data(), size);
    const auto upload = UploadPersistentBytes({uniform_scratch.data(), size});
    descriptor_queue->AddConstantBuffer(upload.address, size, upload.identity);
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS BufferCacheRuntime::UploadPersistentIndexMap(
    std::span<const PersistentBindingRow> rows) {
    const auto* data = reinterpret_cast<const u8*>(rows.data());
    return UploadPersistentBytes({data, rows.size_bytes()}).address;
}

template class VideoCommon::BufferCache<BufferCacheParams>;

} // namespace D3D12
