// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <span>
#include "common/cityhash.h"
#include "common/write_combined_copy.h"
#include "video_core/renderer_d3d12/d3d12_uniform_stream.h"

namespace D3D12 {
// Exact immutable snapshots, bounded to 512 KiB, valid only in the current submission.
// A CPU hash is only an index: equality always checks all bytes. GPU-written buffers never enter.
class UniformContentCache {
public:
    static constexpr size_t MAX_SIZE = 65536;
    struct Stats { u64 hits{}, misses{}, saved_bytes{}; };
    template <typename Allocate>
    UniformUpload Upload(std::span<const u8> bytes, u64 current_tick, Allocate&& allocate) {
        if (tick != current_tick) {
            heads.fill(-1);
            used = entries_used = 0;
            tick = current_tick;
        }
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const size_t bucket = hash % heads.size();
        for (s32 i = heads[bucket]; i >= 0; i = entries[i].next) {
            const auto& e = entries[i];
            if (e.hash == hash && e.size == bytes.size() &&
                std::memcmp(data.data() + e.offset, bytes.data(), bytes.size()) == 0) {
                ++stats.hits;
                stats.saved_bytes += bytes.size();
                return e.upload;
            }
        }
        ++stats.misses;
        auto upload = allocate(bytes.size());
        Common::CopyToWriteCombined(upload.mapped.data(), bytes.data(), bytes.size());
        if (bytes.size() <= MAX_SIZE && bytes.size() <= data.size() - used &&
            entries_used < entries.size()) {
            std::memcpy(data.data() + used, bytes.data(), bytes.size());
            entries[entries_used] = {hash, used, bytes.size(), upload, heads[bucket]};
            heads[bucket] = static_cast<s32>(entries_used++);
            used += bytes.size();
        }
        return upload;
    }
    Stats GetStats() const { return stats; }
    void Invalidate() { tick = ~0ull; }
private:
    struct Entry { u64 hash; size_t offset, size; UniformUpload upload; s32 next; };
    std::array<u8, 512 * 1024> data{};
    std::array<Entry, 2048> entries{};
    std::array<s32, 2048> heads{};
    size_t used{}, entries_used{};
    u64 tick{~0ull};
    Stats stats;
};
} // namespace D3D12
