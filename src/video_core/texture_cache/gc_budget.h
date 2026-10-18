// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace VideoCommon {

// Backend policies specify evictions, not visits. Protected images and readbacks not ready yet
// must not consume that budget and hide reclaimable images later in the LRU. Bound the scan
// separately, including emergency passes without a time limit. Other backends keep visit counting.
// Based on the GC scan/eviction distinction in ProsperoEden's prepare-vulkan-port.py at a26ba788.
class TextureGcBudget {
public:
    static constexpr std::size_t MaxScanned = 8192;

    void BeginPass(std::size_t iterations, bool count_evictions) {
        remaining = iterations;
        scanned = 0;
        evictions = count_evictions;
    }

    [[nodiscard]] bool Visit() {
        if (remaining == 0 || scanned == MaxScanned) {
            return false;
        }
        ++scanned;
        if (!evictions) {
            --remaining;
        }
        return true;
    }

    void Evicted() {
        if (evictions && remaining != 0) {
            --remaining;
        }
    }

    void Reduce(unsigned shift) { remaining >>= shift; }

private:
    std::size_t remaining{};
    std::size_t scanned{};
    bool evictions{};
};

} // namespace VideoCommon
