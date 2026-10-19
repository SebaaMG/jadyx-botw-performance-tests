// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Page accounting adapted from ProsperoEden a26ba788, headless/texture_partial.h (GPL-3.0-or-later).
#pragma once
#include <algorithm>
#include <span>
#include <utility>
#include <vector>
#include "common/common_types.h"

namespace VideoCommon {
void SetPartialTextureUploads(bool enabled);
bool PartialTextureUploadsEnabled();

// Per-image ownership prevents stale ImageId entries after collection or a title change.
// The cache lock protects this state. An untracked page must be restored before whole-image
// untracking, refresh, deletion or unmapping, so device cached-page counts remain balanced.
struct PartialUploadState {
    bool initialized{};
    std::vector<u64> dirty_pages;

    template <class Memory>
    void Write(Memory& memory, u64 base, u64 bytes, u64 address, u64 length) {
        const u64 begin = (std::max)(base, address);
        const u64 end = (std::min)(base + bytes, address + length);
        if (begin >= end) return;
        for (u64 page = begin >> 12; page <= (end - 1) >> 12; ++page) {
            const auto at = std::lower_bound(dirty_pages.begin(), dirty_pages.end(), page);
            if (at != dirty_pages.end() && *at == page) continue;
            dirty_pages.insert(at, page);
            memory.UpdatePagesCachedCount(page << 12, 4096, -1);
        }
    }
    template <class Memory>
    std::vector<u64> End(Memory& memory) {
        std::vector<u64> pages = std::move(dirty_pages);
        dirty_pages.clear();
        for (u64 page : pages) memory.UpdatePagesCachedCount(page << 12, 4096, 1);
        return pages;
    }
    static std::vector<std::pair<u32, u32>> Ranges(std::span<const u64> pages, u64 base, u64 size) {
        std::vector<std::pair<u32, u32>> ranges;
        for (u64 page : pages) {
            const u64 begin = (std::max)(page << 12, base);
            const u64 end = (std::min)((page + 1) << 12, base + size);
            if (begin >= end) continue;
            if (!ranges.empty() && ranges.back().second == begin - base)
                ranges.back().second = static_cast<u32>(end - base);
            else ranges.emplace_back(static_cast<u32>(begin - base), static_cast<u32>(end - base));
        }
        return ranges;
    }
};
} // namespace VideoCommon
