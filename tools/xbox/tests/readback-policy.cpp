#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#include "core/hle/kernel/optimized_memory_runs.h"
#include "video_core/renderer_d3d12/d3d12_heap_growth_policy.h"
#include "video_core/renderer_d3d12/d3d12_readback_reserve.h"
#include "video_core/renderer_d3d12/d3d12_transfer_spans.h"

void Check(bool valid, const char* message) {
    if (!valid)
        throw std::runtime_error(message);
}

int main() try {
    using namespace D3D12;
    constexpr std::uint64_t MiB = 1ULL << 20;
    ReadbackReserve reserve;
    Check(reserve.Retain(63) == 0 && reserve.Retain(3 * MiB) == 0, "reserve accepted cold sizes");
    std::vector<std::uint64_t> charges;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        auto charge = reserve.Retain(1179648);
        if (charge)
            charges.push_back(charge);
        Check(reserve.Bytes() <= ReadbackReserve::Budget, "reserve exceeded byte budget");
    }
    Check(charges.size() == 7, "reserve capacity does not use committed sizes");
    for (auto charge : charges)
        reserve.Release(charge);
    Check(reserve.Bytes() == 0, "reserve charge leaked");
    Check(ReadbackReserve::Keep(10, 130, false) && !ReadbackReserve::Keep(10, 131, false) &&
              !ReadbackReserve::Keep(10, 9, false) && !ReadbackReserve::Keep(10, 10, true),
          "reserve age boundary");
    Check(ReadbackReserve::Keep(10, 18, false, 1) && !ReadbackReserve::Keep(10, 19, false, 1),
          "one-shot readbacks retained as hot");
    for (const auto headroom : {0U, 8U, 16U, 48U, 64U, 80U, 128U}) {
        reserve.Configure(std::uint64_t{headroom} * MiB, true);
        Check(reserve.Target() == (std::min)(8 * MiB, std::uint64_t{headroom} * MiB / 8),
              "retention did not adapt to headroom");
    }
    reserve.Configure(64 * MiB, true);
    const auto held = reserve.Retain(2 * MiB);
    reserve.Configure(0, true);
    Check(reserve.Bytes() == 2 * MiB && reserve.Target() == 0 && !reserve.Retain(MiB),
          "reserve target reduction lost existing ownership or underflowed");
    reserve.Release(held);
    reserve.Configure(0, false);
    Check(reserve.Target() == ReadbackReserve::Budget,
          "unknown memory imposed a fictitious budget");
    for (std::uint64_t rows = 1; rows <= 512; rows *= 2) {
        for (std::uint64_t depth = 1; depth <= 8; ++depth) {
            Check(ContiguousTransferBytes(2048, 2048, rows * 2048, rows * 2048, depth) ==
                      rows * 2048 * depth,
                  "contiguous volume rejected");
            Check(!ContiguousTransferBytes(2047, 2048, rows * 2047, rows * 2048, depth),
                  "padded rows coalesced");
            Check(!ContiguousTransferBytes(2048, 2048, rows * 2048 + 2048, rows * 2048, depth),
                  "slice gap coalesced");
        }
    }
    Check(!ContiguousTransferBytes(1, 1, std::numeric_limits<std::uint64_t>::max(),
                                   std::numeric_limits<std::uint64_t>::max(), 2),
          "copy size overflow");
    Check(HeapGrowth::Capacity(MiB, 4096, true) == MiB &&
              HeapGrowth::Capacity(MiB, 4096, false) == 64 * MiB &&
              HeapGrowth::Capacity(MiB, 4096, false, 4 * MiB) == 4 * MiB,
          "heap capacity policy");
    Check(HeapGrowth::UnderPressure(64 * MiB, {(5120 - 800) * MiB, 5120 * MiB}, {}) &&
              !HeapGrowth::UnderPressure(64 * MiB, {(5120 - 900) * MiB, 5120 * MiB}, {}),
          "growth margin");
    std::array<std::uint64_t, 4> bitmap{};
    bitmap.fill(std::numeric_limits<std::uint64_t>::max());
    for (std::size_t page = 60; page < 70; ++page)
        bitmap[page / 64] &= ~(1ULL << (page % 64));
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    const auto initialized = Kernel::InitializeUnoptimizedPageRuns(
        bitmap, 50, 32,
        [&](std::size_t offset, std::size_t count) { runs.emplace_back(offset, count); });
    Check(initialized == 10 && runs.size() == 1 &&
              runs[0] == std::pair<std::size_t, std::size_t>{60, 10},
          "page initialization crossed optimized boundaries");
    runs = {{20, 3}, {4, 6}, {10, 10}, {30, 2}};
    Kernel::CoalescePageRuns(runs);
    Check(runs.size() == 2 && runs[0] == std::pair<std::size_t, std::size_t>{4, 19},
          "free blocks not coalesced");
    std::size_t budget = 8192, visited{};
    const auto released =
        Kernel::ReleasePageRun(0, 100, 1, budget, [&](std::size_t, std::size_t count) {
            visited += count;
            return count * 4096;
        });
    Check(released == 8192 && visited == 2 && budget == 0, "free page budget ignored");
    std::cout
        << "PASS: readback reserve, contiguous copies, heap growth and optimized guest page runs\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
