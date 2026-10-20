// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include "common/common_types.h"

namespace D3D12 {

/// Chooses the existing coherent DEFAULT-buffer path, never caches untracked guest bytes.
/// Dirty streaming pages may stop notifying CPU writes. Only SynchronizeBuffer can upload
/// and rearm their tracking. Probe a repeatedly bound range once; back off if written again.
/// A clean range always uses its up-to-date host buffer (CPU/GPU/DMA coherence remains owned
/// by BufferCache). Binding addresses and alignments are still checked by the caller.
class UniformCachePolicy {
public:
    struct Statistics { u64 clean{}, streamed{}, probes{}, backoffs{}; };
    bool PreferCached(u64 address, u32 size, u64 identity, u64 frame, bool cpu_dirty) {
        if (!cpu_dirty) { ++stats.clean; return true; }
        const size_t slot = ((address >> 4) ^ (address >> 16) ^ identity ^ size) % entries.size();
        auto& e = entries[slot];
        if (e.address != address || e.size != size || e.identity != identity) {
            e = {.address = address, .identity = identity, .size = size};
        }
        if (e.probed) {
            e.probed = false;
            e.uses = 0;
            // The whole tracking page can be dirtied by a neighbouring uniform. Treat that
            // conservatively too: avoid repeatedly uploading the page in a dynamic workload.
            if (frame - e.probe_frame < 8) {
                e.retry_frame = frame + 60;
                ++stats.backoffs;
            }
        }
        if (frame >= e.retry_frame && ++e.uses >= 8) {
            e.uses = 0;
            e.probed = true;
            e.probe_frame = frame;
            ++stats.probes;
            return true;
        }
        ++stats.streamed;
        return false;
    }
    Statistics GetStatistics() const { return stats; }

private:
    struct Entry {
        u64 address{}, identity{}, retry_frame{}, probe_frame{};
        u32 size{}, uses{};
        bool probed{};
    };
    std::array<Entry, 256> entries{};
    Statistics stats;
};

} // namespace D3D12
