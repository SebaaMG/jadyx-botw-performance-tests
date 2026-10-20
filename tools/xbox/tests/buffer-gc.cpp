#undef NDEBUG
#include <array>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "video_core/renderer_d3d12/d3d12_cache_policy.h"

struct Entry {
    u64 tick{};
    bool dirty{};
    bool alive{true};
    u64 bytes{64};
};

struct Harness {
    std::vector<Entry> entries;
    u64 now{};
    u64 download_cost{};
    bool fail_download{};
    u64 walks{};
    u64 downloads{};
    u64 erased{};

    VideoCommon::BufferGcStatistics Run(VideoCommon::BufferGcPolicy policy, u64 frame) {
        return VideoCommon::CollectBufferGarbage(policy, frame,
            [&](u64 cutoff, auto&& visitor) {
                ++walks;
                for (std::size_t index = 0; index < entries.size(); ++index) {
                    if (entries[index].alive && entries[index].tick <= cutoff && visitor(index)) break;
                }
            },
            [&](std::size_t index) { return entries[index].dirty; },
            [&](std::size_t index) -> u64 {
                auto& entry = entries[index];
                if (!entry.dirty) return 0;
                if (fail_download) throw std::runtime_error("download failed");
                now += download_cost;
                ++downloads;
                entry.dirty = false;
                return entry.bytes;
            },
            [&](std::size_t index) {
                assert(!entries[index].dirty);
                entries[index].alive = false;
                ++erased;
            },
            [&] { return now; });
    }
};

int main() {
    using D3D12::CachePressure;
    using D3D12::CachePressureController;
    const auto normal = CachePressureController::BufferPolicy(CachePressure::Normal, false);
    const auto pressure = CachePressureController::BufferPolicy(CachePressure::Pressure, false);
    const auto critical = CachePressureController::BufferPolicy(CachePressure::Critical, false);
    const auto emergency = CachePressureController::BufferPolicy(CachePressure::Emergency, false);
    const auto recovery = CachePressureController::BufferPolicy(CachePressure::Emergency, true);

    Harness early{{{0, false}, {1, false}}};
    assert(early.Run(normal, 119).passes == 0 && early.walks == 0);
    auto stats = early.Run(normal, 120);
    assert(stats.evictions == 1 && !early.entries[0].alive && early.entries[1].alive);

    Harness clean{{{0, false}, {0, false}, {0, false}, {99, false}}};
    stats = clean.Run(normal, 200);
    assert(stats.visits == 3 && stats.evictions == 3 && stats.downloads == 0);
    assert(clean.entries[3].alive);

    Harness keep_dirty{{{0, true}, {0, false}, {0, true}, {0, false}}};
    stats = keep_dirty.Run(normal, 200);
    assert(stats.dirty_deferred == 2 && stats.downloads == 0 && stats.evictions == 2);
    assert(keep_dirty.entries[0].alive && keep_dirty.entries[2].alive);

    Harness bounded_downloads{{{0, true}, {0, true}, {0, false}, {0, true}}};
    stats = bounded_downloads.Run(pressure, 200);
    assert(stats.downloads == 1 && stats.download_bytes == 64 && stats.dirty_deferred == 2);
    assert(stats.evictions == 2 && bounded_downloads.entries[1].alive);
    stats = bounded_downloads.Run(pressure, 201);
    assert(stats.downloads == 1 && stats.evictions == 1 && stats.dirty_deferred == 1);

    Harness visit_limit;
    visit_limit.entries.resize(100);
    stats = visit_limit.Run(normal, 200);
    assert(stats.visits == normal.iterations && stats.evictions == normal.iterations);

    Harness expired{{{0, true}, {0, false}, {0, true}}};
    expired.download_cost = 5000;
    stats = expired.Run(critical, 200);
    assert(stats.visits == 1 && stats.evictions == 1 && stats.downloads == 1);
    assert(stats.download_us == 5000 && stats.elapsed_us == 5000);
    assert(expired.entries[1].alive && expired.entries[2].alive);

    Harness zero_bytes{{{0, true, true, 0}, {0, true, true, 48}}};
    stats = zero_bytes.Run(pressure, 200);
    assert(stats.evictions == 2 && stats.downloads == 1 && stats.download_bytes == 48);

    Harness exhausted;
    exhausted.entries.assign(100, Entry{0, true});
    exhausted.download_cost = 5000;
    stats = exhausted.Run(recovery, 200);
    assert(stats.downloads == 64 && stats.evictions == 64 && stats.elapsed_us == 320000);
    assert(exhausted.entries[64].alive);

    Harness still_below_limit;
    still_below_limit.entries.assign(100, Entry{0, true});
    stats = still_below_limit.Run(emergency, 200);
    assert(stats.downloads == 1 && stats.evictions == 1 && stats.dirty_deferred == 63);

    Harness failure{{{0, true}}};
    failure.fail_download = true;
    bool threw = false;
    try {
        failure.Run(pressure, 200);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw && failure.erased == 0 && failure.entries[0].alive);

    Harness disabled{{{0, true}, {0, false}}};
    stats = disabled.Run({0, 0, 0, 0}, 200);
    assert(stats.passes == 0 && disabled.walks == 0);

    u64 stress_evictions = 0;
    for (u64 frame = 0; frame < 10000; ++frame) {
        Harness stress;
        for (u64 index = 0; index < 128; ++index) {
            stress.entries.push_back({frame > index ? frame - index : 0, index % 3 == 0,
                                      true, 32 + index});
        }
        const std::array levels{CachePressure::Normal, CachePressure::Pressure,
                                CachePressure::Critical, CachePressure::Emergency};
        const auto policy = CachePressureController::BufferPolicy(levels[frame % levels.size()], false);
        stats = stress.Run(policy, frame);
        assert(stats.visits <= policy.iterations && stats.downloads <= policy.max_downloads);
        for (const auto& entry : stress.entries) {
            if (!entry.alive) assert(frame >= policy.min_age && entry.tick <= frame - policy.min_age);
        }
        stress_evictions += stats.evictions;
    }
    assert(stress_evictions > 0);
    std::cout << "PASS: production buffer GC walker; age/underflow, visits, dirty preservation, "
                 "actual byte counts, bounded downloads/time, exhaustion, failure ordering and "
                 "10000 stress passes; not a GPU fence or FPS measurement\n";
}
