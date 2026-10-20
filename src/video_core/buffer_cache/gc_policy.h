#pragma once

#include <cstddef>

#include "common/common_types.h"

namespace VideoCommon {

struct BufferGcPolicy {
    u64 min_age{};
    std::size_t iterations{};
    u64 time_budget_us{};
    std::size_t max_downloads{};
};

struct BufferGcStatistics {
    u64 passes{};
    u64 visits{};
    u64 evictions{};
    u64 dirty_deferred{};
    u64 downloads{};
    u64 download_bytes{};
    u64 download_us{};
    u64 elapsed_us{};
};

template <typename Walk, typename Dirty, typename Download, typename Erase, typename Clock>
BufferGcStatistics CollectBufferGarbage(const BufferGcPolicy& policy, u64 frame_tick,
                                        Walk&& walk, Dirty&& dirty, Download&& download,
                                        Erase&& erase, Clock&& clock) {
    BufferGcStatistics stats;
    if (frame_tick < policy.min_age || policy.iterations == 0) return stats;
    const u64 started = clock();
    stats.passes = 1;
    walk(frame_tick - policy.min_age, [&](auto buffer_id) {
        if (stats.visits >= policy.iterations ||
            (policy.time_budget_us != 0 && clock() - started >= policy.time_budget_us)) {
            return true;
        }
        ++stats.visits;
        const bool needs_download = dirty(buffer_id);
        if (needs_download && stats.downloads >= policy.max_downloads) {
            ++stats.dirty_deferred;
            return false;
        }
        const u64 download_started = needs_download ? clock() : 0;
        const u64 bytes = download(buffer_id);
        if (bytes != 0) {
            ++stats.downloads;
            stats.download_bytes += bytes;
        }
        if (needs_download) stats.download_us += clock() - download_started;
        erase(buffer_id);
        ++stats.evictions;
        return false;
    });
    stats.elapsed_us = clock() - started;
    return stats;
}

}
