#pragma once

#include <atomic>

namespace VideoCore {

struct SynchronizationOptions {
    bool nonblocking_nv_events{};
    bool consumer_release_fences{};
    bool detached_syncpoint_actions{};
};

inline std::atomic<bool> nonblocking_nv_events{};
inline std::atomic<bool> consumer_release_fences{};
inline std::atomic<bool> detached_syncpoint_actions{};

inline void SetSynchronizationOptions(SynchronizationOptions options) {
    nonblocking_nv_events.store(options.nonblocking_nv_events, std::memory_order_relaxed);
    consumer_release_fences.store(options.consumer_release_fences, std::memory_order_relaxed);
    detached_syncpoint_actions.store(options.detached_syncpoint_actions, std::memory_order_relaxed);
}

inline SynchronizationOptions GetSynchronizationOptions() {
    return {nonblocking_nv_events.load(std::memory_order_relaxed),
            consumer_release_fences.load(std::memory_order_relaxed),
            detached_syncpoint_actions.load(std::memory_order_relaxed)};
}

}
