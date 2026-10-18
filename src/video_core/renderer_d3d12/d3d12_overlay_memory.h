// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <windows.h>
#include <psapi.h>
#include <fmt/format.h>

#include "common/common_types.h"
#include "video_core/renderer_d3d12/d3d12_cache_policy.h"

namespace D3D12 {

// Resident private + shared pages, not private commit or reserved address space.
// K32GetProcessMemoryInfo is available to desktop and UWP apps; no extra library.
inline std::optional<u64> QueryOverlayResidentMemory(HANDLE process = GetCurrentProcess()) {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
        return std::nullopt;
    }
    return static_cast<u64>(counters.WorkingSetSize);
}

inline std::string FormatOverlayResidentMemory(std::optional<u64> bytes) {
    if (!bytes) {
        return "RAM WS --";
    }
    constexpr u64 MiB = 1024ULL * 1024;
    constexpr u64 GiB = 1024 * MiB;
    return *bytes < GiB ? fmt::format("RAM WS {:.0f} MIB", static_cast<double>(*bytes) / MiB)
                        : fmt::format("RAM WS {:.2f} GIB", static_cast<double>(*bytes) / GiB);
}

// Working set is the resident CPU view, not the allocation budget. Show the same usage/limit
// that drives the renderer's guard beside it; a low WS must not look like free commit.
inline std::string FormatOverlayBudgetMemory(std::optional<u64> resident,
                                             const CacheMemorySnapshot& budget) {
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
    const std::string ws = resident ? fmt::format("WS {:.2f}G", *resident / GiB) : "WS --";
    if (!budget.app_limit) return ws + " COMMIT --";
    return fmt::format("{} COMMIT {:.2f}/{:.2f}G", ws, budget.app_used / GiB,
                       budget.app_limit / GiB);
}

} // namespace D3D12
