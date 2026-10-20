#pragma once

#include <cstdint>
#include "video_core/renderer_d3d12/d3d12_heap_packing.h"

namespace D3D12::HeapGrowth {

struct Budget {
    std::uint64_t used{}, limit{};
    std::uint64_t Free() const {
        return used < limit ? limit - used : 0;
    }
};

inline bool UnderPressure(std::uint64_t bytes, Budget app, Budget gpu) {
    const auto tight = [bytes](Budget budget, std::uint64_t margin) {
        const auto free = budget.Free();
        return budget.limit && (bytes >= free || free - bytes <= margin);
    };
    return tight(app, 768ULL << 20) || tight(gpu, gpu.limit / 5);
}

inline std::uint64_t Capacity(std::uint64_t size, std::uint64_t alignment, bool pressure,
                              std::uint64_t normal_block = 64ULL << 20) {
    return HeapPacking::BlockSize(size, alignment, pressure ? 0 : normal_block);
}

} // namespace D3D12::HeapGrowth
