// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "video_core/synchronization_options.h"

namespace Tegra {

namespace Host1x {

class SyncpointManager {
public:
    u32 GetGuestSyncpointValue(u32 id) const {
        return syncpoints_guest[id].load(std::memory_order_acquire);
    }

    u32 GetHostSyncpointValue(u32 id) const {
        return syncpoints_host[id].load(std::memory_order_acquire);
    }

    struct RegisteredAction {
        enum class State { Pending, Running, Completed, Cancelled };
        explicit RegisteredAction(u32 expected_value_, std::function<void()>&& action_)
            : expected_value{expected_value_}, action{std::move(action_)} {}
        u32 expected_value;
        std::function<void()> action;
        std::atomic<State> state{State::Pending};
    };
    using ActionHandle = std::shared_ptr<RegisteredAction>;

    struct ActionQueue {
        std::list<ActionHandle> actions;
        std::recursive_mutex execution_guard;
        std::vector<u32> completed_values;
        bool ordered_completion{};
    };

    template <typename Func>
    ActionHandle RegisterGuestAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    template <typename Func>
    ActionHandle RegisterHostAction(u32 syncpoint_id, u32 expected_value, Func&& action) {
        return RegisterAction(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id],
                              expected_value, std::move(action));
    }

    void DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle);

    void DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle);

    void IncrementGuest(u32 syncpoint_id);

    void IncrementHost(u32 syncpoint_id);
    void CompleteGuest(u32 syncpoint_id, u32 value);
    void CompleteHost(u32 syncpoint_id, u32 value);

    void WaitGuest(u32 syncpoint_id, u32 expected_value);

    void WaitHost(u32 syncpoint_id, u32 expected_value);
    /// Cancel blocking waits during emulation teardown without advancing guest counters.
    void CancelWaits();

    /// Pulse restore: moves a syncpoint forward to the values a checkpoint holds, running the
    /// actions and waking the waits they satisfy. Never moves one back.
    void PulseRaise(u32 syncpoint_id, u32 guest_value, u32 host_value);

    bool IsReadyGuest(u32 syncpoint_id, u32 expected_value) const {
        return HasReached(syncpoints_guest[syncpoint_id].load(std::memory_order_acquire), expected_value);
    }

    bool IsReadyHost(u32 syncpoint_id, u32 expected_value) const {
        return HasReached(syncpoints_host[syncpoint_id].load(std::memory_order_acquire), expected_value);
    }

private:
    static bool HasReached(u32 value, u32 target) {
        return static_cast<s32>(value - target) >= 0;
    }
    std::atomic<bool> stopping{};
    const bool detached_actions{VideoCore::GetSynchronizationOptions().detached_syncpoint_actions};
    void Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                   ActionQueue& action_storage);

    ActionHandle RegisterAction(std::atomic<u32>& syncpoint,
                                ActionQueue& action_storage, u32 expected_value,
                                std::function<void()>&& action);

    void DeregisterAction(ActionQueue& action_storage, const ActionHandle& handle);
    void DrainActions(std::atomic<u32>& syncpoint, ActionQueue& action_storage);
    void RunAction(const ActionHandle& action, bool outside_global_lock = false);
    void Complete(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                  ActionQueue& action_storage, u32 value);

    void Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv, u32 expected_value);

    static constexpr size_t NUM_MAX_SYNCPOINTS = 192;

    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_guest{};
    std::array<std::atomic<u32>, NUM_MAX_SYNCPOINTS> syncpoints_host{};

    std::array<ActionQueue, NUM_MAX_SYNCPOINTS> guest_action_storage;
    std::array<ActionQueue, NUM_MAX_SYNCPOINTS> host_action_storage;

    std::mutex guard;
    std::condition_variable wait_guest_cv;
    std::condition_variable wait_host_cv;
};

} // namespace Host1x

} // namespace Tegra
