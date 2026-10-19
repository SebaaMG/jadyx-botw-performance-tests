// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <optional>
#include <utility>
#include <cstddef>
#include "common/common_types.h"

namespace VideoCommon {
void SetBindingDependencyReuse(bool enabled);
bool BindingDependencyReuseEnabled();

// Validate the descriptor's live words every draw (even when its page is not tracked).
// Mapping generations cover Map, MapSparse and Unmap; memory-manager identity separates channels.
// Only immutable address/size metadata is stored, never a resource ID or a synchronization result.
struct StoragePlanKey {
    u64 address_space{};
    u64 mapping_generation{}, descriptor_address{}, address{}, size_word{};
    u32 alignment{};
    bool written{};
    bool operator==(const StoragePlanKey&) const = default;
};
template <class Plan>
class StoragePlanCache {
public:
    const Plan* Find(const StoragePlanKey& key) const {
        const auto& entry = entries[Index(key)];
        return entry && entry->first == key ? &entry->second : nullptr;
    }
    void Insert(const StoragePlanKey& key, const Plan& plan) {
        entries[Index(key)] = std::pair{key, plan};
    }
private:
    static size_t Index(const StoragePlanKey& key) {
        return ((key.descriptor_address >> 4) ^ (key.descriptor_address >> 12)) & 255;
    }
    std::array<std::optional<std::pair<StoragePlanKey, Plan>>, 256> entries;
};
} // namespace VideoCommon
