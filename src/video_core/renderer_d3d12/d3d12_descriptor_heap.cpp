// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <tuple>

#include <fmt/format.h>

#include "common/bug_tracker.h"
#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_log.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"

namespace D3D12 {

namespace {

// Allocating/freeing any offline view invalidates cached handles, including recycled slots.
// Descriptor creation follows Allocate before another guest table can consume the slot.
std::atomic<u64> offline_view_epoch{1};
std::atomic<bool> persistent_resources_requested{false};

ComPtr<ID3D12DescriptorHeap> CreateHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                        u32 count, bool shader_visible) {
    if (count == 0) {
        throw std::invalid_argument("D3D12: descriptor heap size must not be zero");
    }
    if (shader_visible && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
        throw std::invalid_argument("D3D12: RTV/DSV heaps cannot be shader visible");
    }
    if (shader_visible && type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
        count > D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE) {
        throw std::invalid_argument("D3D12: shader-visible sampler heap exceeds 2048 slots");
    }
    if (shader_visible && type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        count > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1) {
        throw std::invalid_argument(
            "D3D12: shader-visible CBV/SRV/UAV heap exceeds one million slots");
    }
    const D3D12_DESCRIPTOR_HEAP_DESC desc{
        .Type = type,
        .NumDescriptors = count,
        .Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                : D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowIfFailed(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)),
                  "CreateDescriptorHeap");
    return heap;
}

} // Anonymous namespace

void SetPersistentResources(bool enabled) {
    persistent_resources_requested.store(enabled, std::memory_order_relaxed);
}

bool PersistentResourcesSupported(ID3D12Device* device) {
    if (!persistent_resources_requested.load(std::memory_order_relaxed)) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options,
                                                sizeof(options))) &&
           options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
}

struct DescriptorRing::PersistentCache {
    static constexpr u32 NONE = ~0u;
    struct Hash {
        size_t operator()(const PersistentDescriptorKey& k) const noexcept {
            return Common::CityHash64(reinterpret_cast<const char*>(k.words.data()), sizeof(k));
        }
    };
    struct Entry { PersistentDescriptorKey key; u64 tick{}; u32 prev{NONE}, next{NONE}; };
    std::vector<Entry> entries{EDEN_PERSISTENT_RESOURCE_CAPACITY};
    std::vector<u32> free;
    std::unordered_map<PersistentDescriptorKey, u32, Hash> slots;
    u32 oldest{NONE}, newest{NONE};
    PersistentDescriptorStats stats;

    PersistentCache() {
        slots.reserve(EDEN_PERSISTENT_RESOURCE_CAPACITY);
        free.reserve(EDEN_PERSISTENT_RESOURCE_CAPACITY);
        for (u32 i = EDEN_PERSISTENT_RESOURCE_CAPACITY; i-- > 0;) free.push_back(i);
    }
    void Unlink(u32 i) {
        auto& e = entries[i];
        if (e.prev != NONE) entries[e.prev].next = e.next; else oldest = e.next;
        if (e.next != NONE) entries[e.next].prev = e.prev; else newest = e.prev;
    }
    void Touch(u32 i, u64 tick) {
        auto& e = entries[i];
        e.tick = tick;
        e.prev = newest;
        e.next = NONE;
        if (newest != NONE) entries[newest].next = i; else oldest = i;
        newest = i;
    }
};

// --- CpuDescriptorAllocator -------------------------------------------------------------------

CpuDescriptorAllocator::CpuDescriptorAllocator(ID3D12Device* device_,
                                               D3D12_DESCRIPTOR_HEAP_TYPE type_, u32 page_size_,
                                               const Scheduler* read_by_list_calls)
    : device{device_}, type{type_}, page_size{page_size_},
      stride{device_->GetDescriptorHandleIncrementSize(type_)}, scheduler{read_by_list_calls} {
    if (page_size == 0) {
        throw std::invalid_argument("D3D12: offline descriptor page size must not be zero");
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorAllocator::Allocate() {
    std::scoped_lock lock{mutex};
    ReclaimRecorded();
    if (free_list.empty()) {
        AddPage();
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = free_list.back();
    free_list.pop_back();
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        offline_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    return handle;
}

void CpuDescriptorAllocator::Free(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (handle.ptr == 0) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        offline_view_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    if (scheduler) {
        // A call recorded before the free may still have to read the descriptor.
        const u64 point = scheduler->RecordPoint();
        if (!scheduler->IsRecorded(point)) {
            pending_frees.push_back({point, handle});
            return;
        }
    }
    free_list.push_back(handle);
}

void CpuDescriptorAllocator::ReclaimRecorded() {
    while (!pending_frees.empty() && scheduler->IsRecorded(pending_frees.front().record_point)) {
        free_list.push_back(pending_frees.front().handle);
        pending_frees.pop_front();
    }
}

void CpuDescriptorAllocator::AddPage() {
    ComPtr<ID3D12DescriptorHeap> page = CreateHeap(device, type, page_size, false);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = page->GetCPUDescriptorHandleForHeapStart();
    // Pushed in reverse so slots are handed out in ascending order.
    for (u32 i = page_size; i-- > 0;) {
        free_list.push_back({base.ptr + static_cast<SIZE_T>(i) * stride});
    }
    pages.push_back(std::move(page));
    LOG_INFO(Render, "D3D12: offline descriptor page created (type {}, {} slots)",
             static_cast<u32>(type), page_size);
}

// --- DescriptorRing ---------------------------------------------------------------------------

DescriptorRing::DescriptorRing(ID3D12Device* device_, Scheduler& scheduler_, u32 capacity_)
    : device{device_}, scheduler{scheduler_},
      heap{CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                     capacity_ + (PersistentResourcesSupported(device_) ?
                                  3 * EDEN_PERSISTENT_RESOURCE_CAPACITY : 0), true)},
      cpu_base{heap->GetCPUDescriptorHandleForHeapStart()},
      gpu_base{heap->GetGPUDescriptorHandleForHeapStart()}, capacity{capacity_},
      stride{device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)} {
    if (PersistentResourcesSupported(device_)) persistent = std::make_unique<PersistentCache>();
    LOG_INFO(Render, "D3D12: shader-visible descriptor ring ready ({} slots)", capacity);
}

DescriptorRing::~DescriptorRing() = default;

void DescriptorRing::ReservePersistent(u32 count) {
    if (!persistent || count > EDEN_PERSISTENT_RESOURCE_CAPACITY)
        throw std::runtime_error("D3D12: invalid persistent descriptor reservation");
    auto& cache = *persistent;
    // Reserve the worst-case number of misses before any of this draw's uploads/commands.
    // LRU order also orders last-use ticks; a slot is overwritten only after its GPU fence.
    while (cache.free.size() < count) {
        const u32 i = cache.oldest;
        if (i == PersistentCache::NONE) throw std::logic_error("persistent descriptor LRU empty");
        const u64 tick = cache.entries[i].tick;
        if (!scheduler.IsKnownFree(tick) && !scheduler.IsFree(tick)) {
            ++cache.stats.waits;
            scheduler.Wait(tick);
        }
        cache.Unlink(i);
        cache.slots.erase(cache.entries[i].key);
        cache.free.push_back(i);
        ++cache.stats.evictions;
    }
}

std::pair<u32, bool> DescriptorRing::PersistentSlot(const PersistentDescriptorKey& key) {
    auto& cache = *persistent;
    if (const auto found = cache.slots.find(key); found != cache.slots.end()) {
        const u32 i = found->second;
        cache.Unlink(i);
        cache.Touch(i, scheduler.CurrentTick());
        ++cache.stats.hits;
        return {i, false};
    }
    if (cache.free.empty()) throw std::logic_error("persistent descriptors not reserved");
    const u32 i = cache.free.back();
    cache.free.pop_back();
    cache.entries[i].key = key;
    cache.Touch(i, scheduler.CurrentTick());
    cache.slots.emplace(key, i);
    ++cache.stats.misses;
    return {i, true};
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorRing::PersistentCpu(u32 bank, u32 slot) const {
    return {cpu_base.ptr + (static_cast<SIZE_T>(capacity) +
            bank * EDEN_PERSISTENT_RESOURCE_CAPACITY + slot) * stride};
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorRing::PersistentGpu() const {
    return {gpu_base.ptr + static_cast<u64>(capacity) * stride};
}

PersistentDescriptorStats DescriptorRing::GetPersistentStats() const {
    return persistent ? persistent->stats : PersistentDescriptorStats{};
}

void DescriptorRing::TouchPersistent(u32 slot) {
    auto& cache = *persistent;
    cache.Unlink(slot);
    cache.Touch(slot, scheduler.CurrentTick());
}

DescriptorRange DescriptorRing::Allocate(u32 count) {
    if (count == 0 || count > capacity) {
        throw std::runtime_error(fmt::format("D3D12: bad descriptor ring request ({})", count));
    }
    u32 offset{};
    while (!TryAllocate(count, offset)) {
        // Full: wait for the oldest range (flushing it first if it is still being recorded).
        LOG_DEBUG(Render, "D3D12: descriptor ring full, waiting for tick {}",
                  in_flight.front().first);
        scheduler.Wait(in_flight.front().first);
    }
    const u64 tick = scheduler.CurrentTick();
    if (!in_flight.empty() && in_flight.back().first == tick) {
        in_flight.back().second = head;
    } else {
        in_flight.emplace_back(tick, head);
    }
    return {
        .cpu = {cpu_base.ptr + static_cast<SIZE_T>(offset) * stride},
        .gpu = {gpu_base.ptr + static_cast<u64>(offset) * stride},
    };
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorRing::Upload(
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors) {
    if (descriptors.size() > std::numeric_limits<u32>::max()) {
        throw std::length_error("D3D12: descriptor upload is too large");
    }
    const DescriptorRange range = Allocate(static_cast<u32>(descriptors.size()));
    // One destination range, single-descriptor source ranges (null sizes).
    const UINT num = static_cast<UINT>(descriptors.size());
    device->CopyDescriptors(1, &range.cpu, &num, num, descriptors.data(), nullptr,
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("copying {} view descriptors to the shader-visible heap",
                           descriptors.size());
    });
    if (!logged_upload) {
        LOG_INFO(Render, "D3D12: descriptor ring upload active ({} descriptors, tick {})",
                 descriptors.size(), scheduler.CurrentTick());
        logged_upload = true;
    }
    return range.gpu;
}

bool DescriptorRing::TryAllocate(u32 count, u32& offset) {
    // Ranges the last known GPU tick already covers are retired first; the fence is only read
    // when they do not make room, instead of once per draw.
    Retire(false);
    if (FindSpace(count, offset)) {
        head = offset + count;
        return true;
    }
    Retire(true);
    if (FindSpace(count, offset)) {
        head = offset + count;
        return true;
    }
    return false;
}

bool DescriptorRing::FindSpace(u32 count, u32& offset) const {
    // A drained ring keeps its head (tail caught up with it) instead of restarting at 0: a draw may
    // still be writing the table it took when a mid-draw Finish retired every range, and its slots
    // sit right behind the head, so only a whole lap of the ring could reach them again.
    const bool empty = in_flight.empty();
    if (empty || head > tail) {
        // Free space is [head, capacity) and then [0, tail).
        if (capacity - head >= count) {
            offset = head;
        } else if (empty || count < tail) {
            offset = 0; // wrap; the skipped end is reclaimed with the range before it
        } else {
            return false;
        }
        return true;
    }
    if (head < tail && tail - head > count) {
        offset = head;
        return true;
    }
    return false; // head == tail with ranges in flight: completely full
}

void DescriptorRing::Retire(bool read_fence) {
    while (!in_flight.empty()) {
        const u64 tick = in_flight.front().first;
        if (!scheduler.IsKnownFree(tick) && !(read_fence && scheduler.IsFree(tick))) {
            break;
        }
        tail = in_flight.front().second;
        in_flight.pop_front();
    }
}

// --- GuestDescriptorQueue ---------------------------------------------------------------------

u64 DescriptorRing::CurrentTick() const {
    return scheduler.CurrentTick();
}

GuestDescriptorQueue::GuestDescriptorQueue(ID3D12Device* device_, DescriptorRing& ring_)
    : device{device_}, ring{ring_}, stride{ring_.Stride()} {}

void GuestDescriptorQueue::Acquire(u32 count_, u32 image_count_, const PipelineLayout* layout) {
    FlushCopies();
    persistent_layout = layout;
    persistent_images.clear();
    binding_rows.clear();
    if (layout) {
        ring.ReservePersistent(count_);
        count = count_;
        written = image_count = 0;
        binding_rows.resize(layout->NumBindings());
        range = {{}, ring.PersistentGpu()};
        return;
    }
    if (image_count_ > count_) throw std::invalid_argument("D3D12: split descriptor count");
    count = count_ - image_count_;
    image_count = image_count_;
    image_copies.clear();
    written = 0;
    // One spare slot past the table absorbs writes beyond what the root signature declares.
    range = count_ != 0 ? ring.Allocate(count_ + 1) : DescriptorRange{};
    acquire_tick = ring.CurrentTick();
    image_range = {{range.cpu.ptr + static_cast<SIZE_T>(count + 1) * stride},
                   {range.gpu.ptr + static_cast<UINT64>(count + 1) * stride}};
}

void GuestDescriptorQueue::SetPersistentSamplers(std::span<const u32> slots) {
    const auto bindings = persistent_layout->PersistentSamplerBindings();
    if (bindings.size() != slots.size()) throw std::logic_error("persistent sampler count");
    for (size_t i = 0; i < slots.size(); ++i) binding_rows[bindings[i]].sampler = slots[i];
}

size_t GuestDescriptorQueue::ImageKeyHash::operator()(
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> handles) const noexcept {
    return static_cast<size_t>(Common::CityHash64(reinterpret_cast<const char*>(handles.data()),
                                                 handles.size_bytes()));
}

bool GuestDescriptorQueue::ImageKeyEqual::operator()(
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> lhs,
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> rhs) const noexcept {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(),
        [](auto a, auto b) { return a.ptr == b.ptr; });
}

void GuestDescriptorQueue::AddImageCopy(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
    if (!image_count) {
        AddCopy(descriptor);
        return;
    }
    image_copies.push_back(descriptor);
}

D3D12_GPU_DESCRIPTOR_HANDLE GuestDescriptorQueue::ImageTable() {
    if (!image_count) return {};
    if (image_copies.size() != image_count) {
        throw std::runtime_error("D3D12: image descriptor table count mismatch");
    }
    const u64 tick = ring.CurrentTick();
    const u64 epoch = offline_view_epoch.load(std::memory_order_relaxed);
    if (image_tick != tick || image_epoch != epoch) {
        last_image_keys = {};
        image_tables.clear();
        image_tick = tick;
        image_epoch = epoch;
    }
    const std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> key{image_copies.data(), image_copies.size()};
    // A helper upload may have submitted during Configure. Do not cache/reuse an earlier range.
    const bool cacheable = acquire_tick == tick && key.size() <= 256;
    const auto reuse = [&](D3D12_GPU_DESCRIPTOR_HANDLE table) {
        ++image_stats.hits;
        if (!logged_image_reuse) {
            LOG_INFO(Render, "SeriesX: graphics image descriptor reuse active ({} descriptors)",
                     key.size());
            logged_image_reuse = true;
        }
        return table;
    };
    if (cacheable) {
        if (ImageKeyEqual{}(last_image_keys, key)) {
            return reuse(last_image_table);
        }
        if (const auto found = image_tables.find(key); found != image_tables.end()) {
            last_image_keys = found->first;
            last_image_table = found->second;
            return reuse(last_image_table);
        }
    }
    const UINT n = image_count;
    device->CopyDescriptors(1, &image_range.cpu, &n, n, key.data(), nullptr,
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CheckRemovedAfterDescriptor(device, [] { return std::string{"copying split image table"}; });
    ++image_stats.misses;
    image_stats.copied_descriptors += n;
    if (cacheable) {
        if (image_tables.size() >= 512) {
            last_image_keys = {};
            image_tables.clear();
        }
        auto [entry, inserted] = image_tables.emplace(
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>{key.begin(), key.end()}, image_range.gpu);
        last_image_keys = entry->first;
        last_image_table = entry->second;
    }
    return image_range.gpu;
}

D3D12_GPU_DESCRIPTOR_HANDLE GuestDescriptorQueue::Table() {
    if (IsPersistent()) {
        if (written != count) throw std::logic_error("persistent descriptor count mismatch");
        FinishPersistentImages();
        TouchPersistentUsage();
        return range.gpu;
    }
    FlushCopies();
    if (written != count && !logged_mismatch) {
        LOG_ERROR(Render, "D3D12: descriptor table filled with {} of {} descriptors", written,
                  count);
        logged_mismatch = true;
    }
    return range.gpu;
}

void GuestDescriptorQueue::TouchPersistentUsage() {
    if (IsPersistent())
        for (const auto& row : binding_rows) ring.TouchPersistent(row.resource);
}

D3D12_CPU_DESCRIPTOR_HANDLE GuestDescriptorQueue::Next() {
    if (written >= count) {
        // More bindings than the root signature declares: the spare slot. Reported by Table().
        ++written;
        return {range.cpu.ptr + static_cast<SIZE_T>(count) * stride};
    }
    return {range.cpu.ptr + static_cast<SIZE_T>(written++) * stride};
}

void GuestDescriptorQueue::AddConstantBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size,
                                            u64 identity) {
    if (count == 0) {
        return;
    }
    const D3D12_CONSTANT_BUFFER_VIEW_DESC desc{
        .BufferLocation = address,
        .SizeInBytes = address != 0 ? (size + 255u) & ~255u : 0u,
    };
    const auto create = [&](D3D12_CPU_DESCRIPTOR_HANDLE destination) {
        VideoCore::Perf::ScopedNsTimer timer{VideoCore::Perf::Counter::CbvCreateNs};
        VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::CbvCreates, 1);
        device->CreateConstantBufferView(&desc, destination);
    };
    if (IsPersistent()) {
        const PersistentDescriptorKey key{{1, address, desc.SizeInBytes,
            identity ? identity : (address ? ++unknown_identity : 0)}};
        const auto [slot, created] = ring.PersistentSlot(key);
        if (created) create(ring.PersistentCpu(0, slot));
        binding_rows[persistent_layout->PersistentSlots()[written++].binding].resource = slot;
        return;
    }
    create(Next());
    if (GpuDiagnosticsEnabled()) {
        TraceGpuDetail(fmt::format("CBV table={:x} slot={} address={:x} size={}",
            range.gpu.ptr, written - 1, address, desc.SizeInBytes));
    }
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("CBV at 0x{:x} of {} bytes", desc.BufferLocation, desc.SizeInBytes);
    });
}

void GuestDescriptorQueue::AddStorageBuffer(ID3D12Resource* resource, u64 offset, u32 size,
                                           u64 identity) {
    if (GpuDiagnosticsEnabled()) {
        TraceGpuDetail(fmt::format("storage buffer table={:x} slot={} resource={} offset={} size={}",
            range.gpu.ptr, written, static_cast<void*>(resource), offset, size));
    }
    if (count == 0) {
        return;
    }
    // Raw views address 32-bit words from a 16-byte aligned offset.
    if (resource && offset % D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT != 0) {
        BUG_TRACK(UnsupportedState, "storage buffer at an offset not aligned to 16 bytes ({}) bound "
                                    "as null",
                  offset);
        WarnOnceLog(logged_unaligned, "storage buffer at unaligned offset {} bound as null",
                    offset);
        resource = nullptr;
    }
    const u32 words = (size + 3) / 4;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
        .Buffer = {.FirstElement = resource ? offset / 4 : 0,
                   .NumElements = resource ? words : 0,
                   .StructureByteStride = 0,
                   .CounterOffsetInBytes = 0,
                   .Flags = D3D12_BUFFER_UAV_FLAG_RAW},
    };
    const bool persistent_mode = IsPersistent();
    u32 slot{};
    bool created = true;
    if (persistent_mode) {
        const PersistentDescriptorKey key{{2, reinterpret_cast<u64>(resource), offset, size,
            identity ? identity : (resource ? ++unknown_identity : 0)}};
        std::tie(slot, created) = ring.PersistentSlot(key);
    }
    if (created) device->CreateUnorderedAccessView(resource, nullptr, &uav,
                         persistent_mode ? ring.PersistentCpu(2, slot) : Next());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        .Format = DXGI_FORMAT_R32_TYPELESS,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {.FirstElement = resource ? offset / 4 : 0,
                   .NumElements = resource ? words : 0,
                   .StructureByteStride = 0,
                   .Flags = D3D12_BUFFER_SRV_FLAG_RAW},
    };
    if (created) device->CreateShaderResourceView(resource, &srv,
                         persistent_mode ? ring.PersistentCpu(1, slot) : Next());
    if (persistent_mode) {
        binding_rows[persistent_layout->PersistentSlots()[written].binding].resource = slot;
        written += 2;
    }
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("storage buffer views (offset {} size {} null {})", offset, size,
                           resource == nullptr);
    });
}

void GuestDescriptorQueue::AddTexelBuffer(ID3D12Resource* resource, u64 offset, u32 size,
                                          DXGI_FORMAT format, u32 element_size, bool with_uav,
                                          u64 identity) {
    if (GpuDiagnosticsEnabled()) {
        TraceGpuDetail(fmt::format("texel buffer table={:x} slot={} resource={} offset={} size={} format={} uav={}",
            range.gpu.ptr, written, static_cast<void*>(resource), offset, size,
            static_cast<u32>(format), with_uav));
    }
    if (count == 0) {
        return;
    }
    if (format == DXGI_FORMAT_UNKNOWN || element_size == 0) {
        format = DXGI_FORMAT_R32_UINT;
        element_size = 4;
        resource = nullptr;
    }
    if (resource && offset % element_size != 0) {
        BUG_TRACK(UnsupportedState, "texel buffer at an offset not aligned to its element ({} % {}) "
                                    "bound as null",
                  offset, element_size);
        WarnOnceLog(logged_unaligned, "texel buffer at unaligned offset {} bound as null", offset);
        resource = nullptr;
    }
    const u64 first = resource ? offset / element_size : 0;
    const u32 elements = resource ? size / element_size : 0;
    const bool persistent_mode = IsPersistent();
    u32 slot{};
    bool created = true;
    if (persistent_mode) {
        const PersistentDescriptorKey key{{3, reinterpret_cast<u64>(resource), offset, size,
            identity ? identity : (resource ? ++unknown_identity : 0),
            static_cast<u64>(format), element_size, with_uav}};
        std::tie(slot, created) = ring.PersistentSlot(key);
    }
    if (with_uav) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{
            .Format = format,
            .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
            .Buffer = {.FirstElement = first, .NumElements = elements},
        };
        if (created) device->CreateUnorderedAccessView(resource, nullptr, &uav,
                            persistent_mode ? ring.PersistentCpu(2, slot) : Next());
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{
        .Format = format,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {.FirstElement = first, .NumElements = elements},
    };
    if (created) device->CreateShaderResourceView(resource, &srv,
                         persistent_mode ? ring.PersistentCpu(1, slot) : Next());
    if (persistent_mode) {
        binding_rows[persistent_layout->PersistentSlots()[written].binding].resource = slot;
        written += with_uav ? 2 : 1;
    }
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("texel buffer views (format {} offset {} elements {} uav {} null {})",
                           static_cast<u32>(format), offset, elements, with_uav,
                           resource == nullptr);
    });
}

void GuestDescriptorQueue::AddCopy(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
    if (IsPersistent()) {
        const auto plan = persistent_layout->PersistentSlots()[written++];
        auto image = std::ranges::find(persistent_images, plan.binding, &PersistentImage::binding);
        if (image == persistent_images.end()) {
            persistent_images.push_back({plan.binding});
            image = persistent_images.end() - 1;
        }
        if (plan.type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) image->uav = descriptor;
        else image->srv = descriptor;
        return;
    }
    if (GpuDiagnosticsEnabled()) {
        TraceGpuDetail(fmt::format("descriptor copy table={:x} slot={} source={:x}",
            range.gpu.ptr, written, descriptor.ptr));
    }
    if (count == 0) {
        return;
    }
    VideoCore::Perf::AddDetailed(VideoCore::Perf::Counter::ViewCopies, 1);
    if (written >= count) {
        // The spare slot (see Next) is not contiguous with the run: copy it now.
        FlushCopies();
        device->CopyDescriptorsSimple(1, Next(), descriptor,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE slot = Next();
    if (!pending_copies.empty() &&
        slot.ptr != copy_destination.ptr + pending_copies.size() * stride) {
        FlushCopies(); // a view was created in between: start a new run
    }
    if (pending_copies.empty()) {
        copy_destination = slot;
    }
    pending_copies.push_back(descriptor);
}

void GuestDescriptorQueue::FinishPersistentImages() {
    const u64 epoch = offline_view_epoch.load(std::memory_order_relaxed);
    for (const auto& image : persistent_images) {
        const PersistentDescriptorKey key{{4, image.srv.ptr, image.uav.ptr, epoch}};
        const auto [slot, created] = ring.PersistentSlot(key);
        if (created) {
            for (const auto [bank, source] : {std::pair{1u, image.srv},
                                               std::pair{2u, image.uav}}) {
                if (!source.ptr) continue;
                device->CopyDescriptorsSimple(1, ring.PersistentCpu(bank, slot), source,
                                              D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
        }
        binding_rows[image.binding].resource = slot;
    }
    persistent_images.clear();
}

void GuestDescriptorQueue::FlushCopies() {
    if (IsPersistent()) {
        FinishPersistentImages();
        return;
    }
    if (pending_copies.empty()) {
        return;
    }
    VideoCore::Perf::ScopedNsTimer timer{VideoCore::Perf::Counter::ViewCopyNs};
    const UINT num = static_cast<UINT>(pending_copies.size());
    device->CopyDescriptors(1, &copy_destination, &num, num, pending_copies.data(), nullptr,
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CheckRemovedAfterDescriptor(device, [&] {
        return fmt::format("copying {} texture descriptors for a draw", num);
    });
    pending_copies.clear();
}

// --- SamplerHeap ------------------------------------------------------------------------------

size_t SamplerHeap::KeyHash::operator()(std::span<const u64> key) const noexcept {
    return static_cast<size_t>(Common::CityHash64(reinterpret_cast<const char*>(key.data()),
                                                  key.size() * sizeof(u64)));
}

bool SamplerHeap::KeyEqual::operator()(std::span<const u64> lhs,
                                      std::span<const u64> rhs) const noexcept {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

SamplerHeap::SamplerHeap(ID3D12Device* device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_},
      heap{CreateHeap(device_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, CAPACITY, true)},
      cpu_base{heap->GetCPUDescriptorHandleForHeapStart()},
      gpu_base{heap->GetGPUDescriptorHandleForHeapStart()},
      stride{device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)} {
    persistent_partition = PersistentResourcesSupported(device_);
    LOG_INFO(Render, "D3D12: shader-visible sampler heap ready ({} slots)", CAPACITY);
}

D3D12_GPU_DESCRIPTOR_HANDLE SamplerHeap::GetTable(
    std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> samplers) {
    if (samplers.size() > std::numeric_limits<u32>::max()) {
        throw std::length_error("D3D12: sampler table is too large");
    }
    const u32 count = static_cast<u32>(samplers.size());
    if (count == 0 || count > CAPACITY || keys.size() != samplers.size()) {
        throw std::runtime_error(fmt::format("D3D12: bad sampler table request ({})", count));
    }
    // Guest persistent indices occupy the upper half. Blit/present helpers only use the lower
    // half, so their Finish/reset can never invalidate a guest draw still being prepared.
    // An unusually large legacy table may temporarily take the whole heap between guest draws.
    if (persistent_partition && count > CAPACITY / 2) {
        scheduler.Finish();
        last_keys = {};
        tables.clear(); persistent_samplers.clear();
        used = persistent_used = 0;
        persistent_partition = false;
    }
    if (std::ranges::equal(keys, last_keys)) {
        return {gpu_base.ptr + static_cast<u64>(last_first) * stride};
    }
    // Heterogeneous lookup avoids allocating/copying a vector on every draw. Only a new
    // table owns a key; hits compare the caller's span against that stored vector.
    if (const auto it = tables.find(keys); it != tables.end()) {
        if (!logged_reuse) {
            LOG_INFO(Render, "D3D12: sampler table deduplication active ({} samplers)", count);
            logged_reuse = true;
        }
        last_keys = it->first;
        last_first = it->second;
        return {gpu_base.ptr + static_cast<u64>(it->second) * stride};
    }
    const u32 legacy_capacity = persistent_partition ? CAPACITY / 2 : CAPACITY;
    if (used + count > legacy_capacity) {
        // Tables already recorded may still be read by the GPU: only reuse the heap once idle.
        LOG_INFO(Render, "D3D12: sampler heap full ({} tables), resetting", tables.size());
        scheduler.Finish();
        last_keys = {};
        tables.clear();
        used = 0;
    }
    const u32 first = used;
    // One destination range, single-descriptor source ranges (null sizes), as DescriptorRing.
    const D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu_base.ptr + static_cast<SIZE_T>(first) * stride};
    device->CopyDescriptors(1, &dst, &count, count, samplers.data(), nullptr,
                            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    CheckRemovedAfter(device, [&] {
        return fmt::format("copying {} sampler descriptors", samplers.size());
    });
    used += count;
    const auto entry = tables.emplace(std::vector<u64>(keys.begin(), keys.end()), first).first;
    last_keys = entry->first;
    last_first = first;
    if (!logged_cache) {
        LOG_INFO(Render, "D3D12: sampler table cached ({} samplers, {} of {} slots used)", count,
                 used, CAPACITY);
        logged_cache = true;
    }
    return {gpu_base.ptr + static_cast<u64>(first) * stride};
}

SamplerHeap::PersistentIndices SamplerHeap::GetPersistentIndices(
    std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors) {
    if (keys.size() != descriptors.size() || keys.size() > CAPACITY / 2)
        throw std::invalid_argument("persistent sampler reservation");
    if (!persistent_partition) {
        scheduler.Finish();
        last_keys = {};
        tables.clear(); persistent_samplers.clear();
        used = persistent_used = 0;
        persistent_partition = true;
    }
    // Conservatively reserve for every key, including duplicates. Reset before any draw upload.
    u32 missing{};
    for (u64 key : keys) missing += !persistent_samplers.contains(key);
    if (persistent_used + missing > CAPACITY / 2) {
        scheduler.Finish();
        persistent_samplers.clear();
        persistent_used = 0;
    }
    PersistentIndices indices;
    indices.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        auto [entry, inserted] = persistent_samplers.try_emplace(keys[i], CAPACITY / 2 + persistent_used);
        if (inserted) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dst{cpu_base.ptr +
                static_cast<SIZE_T>(CAPACITY / 2 + persistent_used++) * stride};
            device->CopyDescriptorsSimple(1, dst, descriptors[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        indices.push_back(entry->second);
    }
    return indices;
}

} // namespace D3D12
