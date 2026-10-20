// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <vector>
#include <d3d12.h>
#include "common/common_types.h"

namespace D3D12 {

/// Four immutable layout plans, at most 4096 footprints each (~512 KiB total). No resource
/// pointers, pixels or GPU lifetime: only shape/format. Owned by the recording thread.
class CopyFootprintCache {
public:
    struct Plan {
        D3D12_RESOURCE_DESC desc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
        u64 bytes{}, last_use{};
    };
    struct Statistics { u64 hits{}, builds{}; };
    const Plan& Get(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, bool enabled,
                    Plan& uncached) {
        const u32 count = desc.MipLevels *
            (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1U : desc.DepthOrArraySize);
        if (!enabled || count > 4096) {
            Build(device, desc, count, uncached);
            return uncached;
        }
        ++clock;
        auto* oldest = &plans[0];
        for (auto& plan : plans) {
            if (!plan.footprints.empty() && SameLayout(plan.desc, desc)) {
                ++stats.hits;
                plan.last_use = clock;
                return plan;
            }
            if (plan.last_use < oldest->last_use) oldest = &plan;
        }
        Build(device, desc, count, *oldest);
        oldest->last_use = clock;
        return *oldest;
    }
    Statistics GetStatistics() const { return stats; }

private:
    static bool SameLayout(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
        return a.Dimension == b.Dimension && a.Alignment == b.Alignment && a.Width == b.Width &&
            a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
            a.MipLevels == b.MipLevels && a.Format == b.Format &&
            a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality &&
            a.Layout == b.Layout && a.Flags == b.Flags;
    }
    void Build(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc, u32 count, Plan& plan) {
        plan.desc = desc;
        plan.footprints.resize(count);
        device->GetCopyableFootprints(&desc, 0, count, 0, plan.footprints.data(), nullptr,
                                     nullptr, &plan.bytes);
        ++stats.builds;
    }
    std::array<Plan, 4> plans;
    u64 clock{};
    Statistics stats;
};

} // namespace D3D12
