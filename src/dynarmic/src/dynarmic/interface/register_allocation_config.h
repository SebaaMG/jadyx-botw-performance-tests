// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace Dynarmic {

// Optional code-generation diagnostics. These count emitted operations, not executions.
// The owner must keep this object alive until its Jit is destroyed, and may read it only
// while that Jit is stopped. No counters are added to the generated guest instructions.
struct RegisterAllocationStatistics {
    std::uint64_t free_gpr_moves{};
    std::uint64_t free_gpr_moves_to_r13{};
    std::uint64_t gpr_stack_spills{};
    std::uint64_t xmm_stack_spills{};
    std::uint64_t r13_selections{};
};

// x64 only. Snapshotted in UserConfig before compiling any blocks; both default to
// the original allocator. R13 remains reserved whenever the emitter reserves it.
struct RegisterAllocationConfig {
    bool free_gpr_moves{};
    bool use_r13{};
    RegisterAllocationStatistics* statistics{};
};

} // namespace Dynarmic
