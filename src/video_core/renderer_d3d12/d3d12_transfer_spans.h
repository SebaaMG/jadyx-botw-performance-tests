#pragma once

#include <cstdint>
#include <limits>

namespace D3D12 {

constexpr std::uint64_t ContiguousTransferBytes(std::uint64_t row_bytes, std::uint64_t row_pitch,
                                                std::uint64_t tight_slice,
                                                std::uint64_t padded_slice, std::uint64_t depth) {
    if (!row_bytes || row_bytes != row_pitch || !tight_slice || tight_slice != padded_slice ||
        !depth || tight_slice > std::numeric_limits<std::uint64_t>::max() / depth)
        return 0;
    return tight_slice * depth;
}

} // namespace D3D12
