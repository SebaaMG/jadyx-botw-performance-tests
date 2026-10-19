// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Store-only grouping adapted from ProsperoEden a26ba788, headless/CMakeLists.txt
// (GPL-3.0-or-later). Packet decoding preserves the original Jadyx scalar path.
#pragma once

#include <algorithm>
#include <span>
#include "video_core/dma_pusher.h"

namespace Tegra::Detail {

struct PacketOptions {
    bool batch{};
    u32 first_constant_data{};
};

// A packet starting below CB_DATA can enter it later. This is only an eligibility hint:
// CallConstBufferRun still checks the live engine, shadow mode and exact remaining range.
inline bool PacketMayHaveConstants(u32 first, u32 method, u32 count, bool once) {
    return count >= 2 && (once ? method >= first && method < first + 16
                              : method < first + 16 && u64(method) + count > first);
}

template <typename Engine>
u32 AppendStateRegisters(Engine* engine, std::span<const CommandHeader> commands, u32 method,
                         u32 pending, bool once) {
    if (!engine || once || method < 0x40 || method >= engine->execution_mask.size()) return 0;
    const u32 limit = static_cast<u32>(std::min({std::size_t(pending), commands.size(),
                                                engine->execution_mask.size() - method}));
    u32 run = 0;
    while (run < limit && !engine->execution_mask[method + run]) ++run;
    if (run == 0) return 0;
    auto& sink = engine->method_sink;
    const std::size_t needed = sink.size() + run;
    if (needed > sink.capacity()) sink.reserve(std::max(needed, sink.capacity() * 2));
    for (u32 i = 0; i < run; ++i) sink.emplace_back(method + i, commands[i].argument);
    return run;
}

// Dispatch supplies engine side effects; this is the actual production packet decoder.
// Kept independent of Core::System lifetime so scalar/batched traces can be compared.
template <bool Batch, typename Dispatch>
void DecodePacketsImpl(DmaPusher::DmaState& state, bool& once, bool& may_have_constants,
                   std::span<const CommandHeader> commands, PacketOptions options,
                   Dispatch& dispatch) {
    const auto set_state = [&](const CommandHeader& header, bool increase_once) {
        state.method = header.method;
        state.subchannel = header.subchannel;
        state.method_count = header.method_count;
        may_have_constants = !Batch || PacketMayHaveConstants(
            options.first_constant_data, state.method, state.method_count, increase_once);
    };
    for (std::size_t index = 0; index < commands.size();) {
        if (state.method_count && state.non_incrementing) {
            state.dma_word_offset = u32(index * sizeof(u32));
            const u32 words = static_cast<u32>(std::min<std::size_t>(
                state.method_count, commands.size() - index));
            dispatch.Multi(&commands[index].argument, words);
            state.method_count -= words;
            state.is_last_call = true;
            index += words;
        } else if (state.method_count) {
            const bool in_constant_range =
                u32(state.method - options.first_constant_data) < 16;
            if ((!Batch || (may_have_constants && in_constant_range)) &&
                state.method_count >= 2) {
                if (const u32 words = dispatch.Constants(commands.subspan(index), index); words) {
                    index += words;
                    continue;
                }
            }
            if constexpr (Batch) {
                if (const u32 words = dispatch.StateRun(commands.subspan(index)); words) {
                    state.dma_word_offset = u32((index + words - 1) * sizeof(u32));
                    state.method += words;
                    state.method_count -= words;
                    state.is_last_call = state.method_count == 0;
                    index += words;
                    continue;
                }
            }
            state.dma_word_offset = u32(index * sizeof(u32));
            state.is_last_call = state.method_count <= 1;
            dispatch.Single(commands[index].argument);
            state.method += !state.non_incrementing ? 1 : 0;
            state.non_incrementing |= once;
            --state.method_count;
            ++index;
        } else {
            const auto header = commands[index];
            switch (header.mode) {
            case SubmissionMode::Increasing:
                set_state(header, false);
                state.non_incrementing = false;
                once = false;
                break;
            case SubmissionMode::NonIncreasing:
                set_state(header, false);
                state.non_incrementing = true;
                once = false;
                break;
            case SubmissionMode::Inline:
                state.method = header.method;
                state.subchannel = header.subchannel;
                state.dma_word_offset = u64(-s64(state.dma_get));
                dispatch.Single(header.arg_count);
                state.non_incrementing = true;
                once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                set_state(header, true);
                state.non_incrementing = false;
                once = true;
                break;
            default:
                break;
            }
            ++index;
        }
    }
}

template <typename Dispatch>
void DecodePackets(DmaPusher::DmaState& state, bool& once, bool& may_have_constants,
                   std::span<const CommandHeader> commands, PacketOptions options,
                   Dispatch& dispatch) {
    // Select once per command buffer. The scalar control has no per-word feature branch.
    if (options.batch) DecodePacketsImpl<true>(state, once, may_have_constants, commands, options, dispatch);
    else DecodePacketsImpl<false>(state, once, may_have_constants, commands, options, dispatch);
}
} // namespace Tegra::Detail
