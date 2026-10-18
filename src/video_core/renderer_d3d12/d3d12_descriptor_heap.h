// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <eden_spirv_to_dxil.h>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;
class PipelineLayout;

void SetPersistentResources(bool enabled);
bool PersistentResourcesSupported(ID3D12Device* device);

struct PersistentDescriptorKey {
    std::array<u64, 8> words{};
    bool operator==(const PersistentDescriptorKey&) const = default;
};

struct PersistentDescriptorStats {
    u64 hits{}, misses{}, evictions{}, waits{};
};

struct PersistentBindingRow {
    u32 resource{}, sampler{}, reserved0{}, reserved1{};
    bool operator==(const PersistentBindingRow&) const = default;
};
static_assert(sizeof(PersistentBindingRow) == 16);

/// Persistent descriptors in non-shader-visible ("offline") heaps: the SRVs/UAVs of image views,
/// RTVs/DSVs of attachments and samplers. Grows in pages; freed slots are reused.
///
/// A slot may be freed as soon as its last use has been recorded: CopyDescriptors and
/// OMSetRenderTargets read descriptors on the CPU timeline, when they are called. The RTVs and
/// DSVs are read by list calls, which a recording worker makes later: given the scheduler, a freed
/// slot is reused only once what was recorded before the free has reached the list.
class CpuDescriptorAllocator {
public:
    explicit CpuDescriptorAllocator(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                    u32 page_size = 1024,
                                    const Scheduler* read_by_list_calls = nullptr);

    CpuDescriptorAllocator(const CpuDescriptorAllocator&) = delete;
    CpuDescriptorAllocator& operator=(const CpuDescriptorAllocator&) = delete;

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Allocate();
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE handle);

private:
    struct PendingFree {
        u64 record_point;
        D3D12_CPU_DESCRIPTOR_HANDLE handle;
    };

    void AddPage();
    /// Moves the slots whose last use reached the list to free_list (under mutex).
    void ReclaimRecorded();

    ID3D12Device* device;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    u32 page_size;
    u32 stride;
    const Scheduler* scheduler;
    std::vector<ComPtr<ID3D12DescriptorHeap>> pages;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> free_list;
    std::deque<PendingFree> pending_frees; ///< in record point order
    std::mutex mutex;
};

struct DescriptorRange {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

/// The one shader-visible CBV/SRV/UAV heap, used as a ring: each draw or dispatch takes a
/// contiguous range for its descriptor tables and copies the offline descriptors into it.
/// Ranges are retired by scheduler tick; a full ring flushes and waits for the oldest range.
class DescriptorRing {
public:
    explicit DescriptorRing(ID3D12Device* device, Scheduler& scheduler, u32 capacity);
    ~DescriptorRing();

    DescriptorRing(const DescriptorRing&) = delete;
    DescriptorRing& operator=(const DescriptorRing&) = delete;

    [[nodiscard]] DescriptorRange Allocate(u32 count);

    /// Allocates count slots and copies the given offline descriptors into them.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Upload(
        std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors);

    [[nodiscard]] ID3D12DescriptorHeap* Heap() const {
        return heap.Get();
    }
    [[nodiscard]] u32 Stride() const {
        return stride;
    }
    [[nodiscard]] u64 CurrentTick() const;
    [[nodiscard]] bool HasPersistent() const { return persistent != nullptr; }
    void ReservePersistent(u32 count);
    [[nodiscard]] std::pair<u32, bool> PersistentSlot(const PersistentDescriptorKey& key);
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE PersistentCpu(u32 bank, u32 slot) const;
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE PersistentGpu() const;
    [[nodiscard]] PersistentDescriptorStats GetPersistentStats() const;
    void TouchPersistent(u32 slot);

private:
    bool TryAllocate(u32 count, u32& offset);
    /// Where count slots fit now, without taking them.
    bool FindSpace(u32 count, u32& offset) const;
    /// Drops the ranges the GPU is done with; only read_fence asks the fence for new progress.
    void Retire(bool read_fence);

    ID3D12Device* device;
    Scheduler& scheduler;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    u32 capacity;
    u32 stride;
    u32 head{}; ///< next free slot
    u32 tail{}; ///< oldest slot the GPU may still read
    std::deque<std::pair<u64, u32>> in_flight; ///< (tick, end offset), oldest first
    bool logged_upload{};
    struct PersistentCache;
    std::unique_ptr<PersistentCache> persistent;
};

/// The CBV/SRV/UAV table of one draw or dispatch, written in root-signature order straight into a
/// range of the ring (see d3d12_root_signature.h for the slot order). Buffer views are created in
/// place; image views are copied from their offline descriptors.
///
/// Acquire() must come before any command of the draw is recorded: allocating from a full ring
/// flushes the command list.
class GuestDescriptorQueue {
public:
    explicit GuestDescriptorQueue(ID3D12Device* device, DescriptorRing& ring);

    GuestDescriptorQueue(const GuestDescriptorQueue&) = delete;
    GuestDescriptorQueue& operator=(const GuestDescriptorQueue&) = delete;

    /// Starts a table of count descriptors (none when count is zero).
    void Acquire(u32 count, u32 image_count = 0, const PipelineLayout* persistent_layout = nullptr);
    [[nodiscard]] bool IsPersistent() const { return persistent_layout != nullptr; }
    void SetPersistentSamplers(std::span<const u32> slots);
    [[nodiscard]] std::span<const PersistentBindingRow> PersistentRows() const { return binding_rows; }
    [[nodiscard]] PersistentDescriptorStats GetPersistentStats() const { return ring.GetPersistentStats(); }
    void TouchPersistentUsage();

    /// GPU handle of the table; checks (and warns once) that it was filled exactly.
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE Table();
    /// Stable image-only table, reused only in the same submission and offline-descriptor epoch.
    void AddImageCopy(D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE ImageTable();
    struct ImageTableStats {
        u64 hits{};
        u64 misses{};
        u64 copied_descriptors{};
    };
    [[nodiscard]] ImageTableStats GetImageTableStats() const { return image_stats; }

    /// A CBV; address 0 writes a null one. size is rounded up to 256 bytes.
    void AddConstantBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size, u64 identity = 0);
    /// A raw UAV, then a raw SRV of the same range (null ones when resource is null).
    void AddStorageBuffer(ID3D12Resource* resource, u64 offset, u32 size, u64 identity = 0);
    /// A typed SRV (texture buffer), preceded by a typed UAV for image buffers. Null views when
    /// resource is null or the offset is not a whole number of elements.
    void AddTexelBuffer(ID3D12Resource* resource, u64 offset, u32 size, DXGI_FORMAT format,
                        u32 element_size, bool with_uav, u64 identity = 0);
    /// Copies of offline descriptors (texture SRVs, image UAVs). Consecutive copies are recorded
    /// with one CopyDescriptors call when the table is finished.
    void AddCopy(D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
    /// Writes the pending copies to the slots reserved for them (Table() also does).
    void FlushCopies();

private:
    D3D12_CPU_DESCRIPTOR_HANDLE Next();
    void FinishPersistentImages();

    ID3D12Device* device;
    DescriptorRing& ring;
    u32 stride;
    DescriptorRange range{};
    u32 count{};
    u32 written{};
    /// Sources of the pending copies, written from copy_destination on.
    boost::container::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 64> pending_copies;
    D3D12_CPU_DESCRIPTOR_HANDLE copy_destination{};
    bool logged_mismatch{};
    bool logged_unaligned{};
    struct ImageKeyHash {
        using is_transparent = void;
        size_t operator()(std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> handles) const noexcept;
    };
    struct ImageKeyEqual {
        using is_transparent = void;
        bool operator()(std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> lhs,
                        std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> rhs) const noexcept;
    };
    // No added GPU heap or cross-submission residency: the original ring still owns every slot.
    std::unordered_map<std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>, D3D12_GPU_DESCRIPTOR_HANDLE,
                       ImageKeyHash, ImageKeyEqual> image_tables;
    boost::container::small_vector<D3D12_CPU_DESCRIPTOR_HANDLE, 64> image_copies;
    DescriptorRange image_range{};
    u32 image_count{};
    u64 acquire_tick{};
    u64 image_tick{};
    u64 image_epoch{};
    std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> last_image_keys;
    D3D12_GPU_DESCRIPTOR_HANDLE last_image_table{};
    ImageTableStats image_stats;
    bool logged_image_reuse{};
    const PipelineLayout* persistent_layout{};
    boost::container::small_vector<PersistentBindingRow, 64> binding_rows;
    struct PersistentImage { u32 binding; D3D12_CPU_DESCRIPTOR_HANDLE srv{}, uav{}; };
    boost::container::small_vector<PersistentImage, 32> persistent_images;
    u64 unknown_identity{};
};

/// The one shader-visible sampler heap. D3D12 caps it at 2048 entries on every tier, too few for
/// a per-draw ring, so whole sampler tables are deduplicated by content: games reuse the same
/// combinations constantly. When the heap fills up it is reset after the GPU goes idle.
class SamplerHeap {
public:
    static constexpr u32 CAPACITY = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

    explicit SamplerHeap(ID3D12Device* device, Scheduler& scheduler);

    SamplerHeap(const SamplerHeap&) = delete;
    SamplerHeap& operator=(const SamplerHeap&) = delete;

    /// Returns a table holding the given offline samplers, in order. keys must identify each
    /// sampler uniquely for the lifetime of the renderer (they are never reused).
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GetTable(
        std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> samplers);

    [[nodiscard]] ID3D12DescriptorHeap* Heap() const {
        return heap.Get();
    }

    /// Transactional reservation: if the heap resets, all indices for this draw are rebuilt.
    using PersistentIndices = boost::container::small_vector<u32, 32>;
    [[nodiscard]] PersistentIndices GetPersistentIndices(
        std::span<const u64> keys, std::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors);
    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE GpuBase() const { return gpu_base; }

private:
    struct KeyHash {
        using is_transparent = void;
        size_t operator()(std::span<const u64> key) const noexcept;
    };
    struct KeyEqual {
        using is_transparent = void;
        bool operator()(std::span<const u64> lhs, std::span<const u64> rhs) const noexcept;
    };

    ID3D12Device* device;
    Scheduler& scheduler;
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base;
    u32 stride;
    u32 used{};
    std::unordered_map<std::vector<u64>, u32, KeyHash, KeyEqual> tables; ///< keys -> first slot
    /// The table the previous call returned: consecutive draws mostly sample the same way, and
    /// comparing against it skips hashing the keys.
    // unordered_map nodes keep their keys stable across rehash. Borrow the immutable key
    // instead of copying it on every non-consecutive cache hit; clear before tables.clear().
    std::span<const u64> last_keys;
    u32 last_first{};
    bool logged_cache{};
    bool logged_reuse{};
    std::unordered_map<u64, u32> persistent_samplers;
    bool persistent_partition{};
    u32 persistent_used{};
};

} // namespace D3D12
