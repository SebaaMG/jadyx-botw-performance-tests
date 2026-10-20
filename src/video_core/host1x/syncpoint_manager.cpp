// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/host1x/syncpoint_manager.h"
#include <algorithm>
#include "common/scope_exit.h"
#include "video_core/frame_trace.h"

namespace Tegra {

namespace Host1x {

namespace {
struct ActiveAction {
    const SyncpointManager::RegisteredAction* entry;
    const ActiveAction* previous;
};
thread_local const ActiveAction* current_action{};
}

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, ActionQueue& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    const auto execute_ready = [&](bool outside_global_lock) {
        std::unique_lock execution_lock{action_storage.execution_guard, std::defer_lock};
        if (detached_actions) execution_lock.lock();
        if (stopping.load(std::memory_order_acquire)) return;
        const VideoCore::FrameTrace::ScopedSpan callback_span{
            VideoCore::FrameTrace::Event::SyncpointCallbackLong,
            (static_cast<u64>(outside_global_lock) << 63) | expected_value};
        action();
    };
    if (stopping.load(std::memory_order_acquire)) return {};
    if (HasReached(syncpoint.load(std::memory_order_acquire), expected_value)) {
        execute_ready(true);
        return {};
    }

    VideoCore::FrameTrace::ScopedSpan lock_span{
        VideoCore::FrameTrace::Event::SyncpointLockLong, expected_value};
    std::unique_lock lk(guard);
    lock_span.Finish();
    if (stopping.load(std::memory_order_acquire)) return {};
    if (HasReached(syncpoint.load(std::memory_order_relaxed), expected_value)) {
        const bool outside_lock = detached_actions || action_storage.ordered_completion;
        if (outside_lock) lk.unlock();
        execute_ready(outside_lock);
        return {};
    }
    auto entry = std::make_shared<RegisteredAction>(expected_value, std::move(action));
    auto it = action_storage.actions.begin();
    while (it != action_storage.actions.end()) {
        if (static_cast<s32>((*it)->expected_value - expected_value) >= 0) {
            break;
        }
        ++it;
    }
    action_storage.actions.insert(it, entry);
    return entry;
}

void SyncpointManager::DeregisterAction(ActionQueue& action_storage,
                                        const ActionHandle& handle) {
    if (!handle) return;
    for (auto* active = current_action; active; active = active->previous) {
        if (active->entry == handle.get()) return;
    }
    {
        std::scoped_lock lk(guard);
        auto expected = RegisteredAction::State::Pending;
        if (handle->state.compare_exchange_strong(expected, RegisteredAction::State::Cancelled)) {
            action_storage.actions.remove(handle);
            handle->state.notify_all();
            return;
        }
    }
    while (handle->state.load(std::memory_order_acquire) == RegisteredAction::State::Running) {
        handle->state.wait(RegisteredAction::State::Running, std::memory_order_acquire);
    }
}

void SyncpointManager::RunAction(const ActionHandle& entry, bool outside_global_lock) {
    auto expected = RegisteredAction::State::Pending;
    if (!entry->state.compare_exchange_strong(expected, RegisteredAction::State::Running)) return;
    const ActiveAction active{entry.get(), current_action};
    current_action = &active;
    SCOPE_EXIT {
        current_action = active.previous;
        entry->state.store(RegisteredAction::State::Completed, std::memory_order_release);
        entry->state.notify_all();
    };
    const VideoCore::FrameTrace::ScopedSpan span{
        VideoCore::FrameTrace::Event::SyncpointCallbackLong,
        (static_cast<u64>(outside_global_lock) << 63) | entry->expected_value};
    entry->action();
}

void SyncpointManager::DrainActions(std::atomic<u32>& syncpoint, ActionQueue& action_storage) {
    std::scoped_lock execution_lock{action_storage.execution_guard};
    while (true) {
        ActionHandle entry;
        {
            std::scoped_lock lk(guard);
            if (stopping.load(std::memory_order_acquire) || action_storage.actions.empty() ||
                !HasReached(syncpoint.load(std::memory_order_acquire),
                            action_storage.actions.front()->expected_value)) return;
            entry = std::move(action_storage.actions.front());
            action_storage.actions.pop_front();
        }
        RunAction(entry, true);
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id]);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id]);
}

void SyncpointManager::CompleteGuest(u32 syncpoint_id, u32 value) {
    Complete(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id], value);
}

void SyncpointManager::CompleteHost(u32 syncpoint_id, u32 value) {
    Complete(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id], value);
}

void SyncpointManager::Complete(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                ActionQueue& action_storage, u32 value) {
    {
        std::scoped_lock lk(guard);
        if (stopping.load(std::memory_order_acquire)) return;
        action_storage.ordered_completion = true;
        u32 current = syncpoint.load(std::memory_order_acquire);
        if (HasReached(current, value)) return;
        auto& completed = action_storage.completed_values;
        if (std::find(completed.begin(), completed.end(), value) == completed.end())
            completed.push_back(value);
        while (true) {
            const auto next = std::find(completed.begin(), completed.end(), current + 1);
            if (next == completed.end()) break;
            ++current;
            completed.erase(next);
        }
        syncpoint.store(current, std::memory_order_release);
        wait_cv.notify_all();
    }
    DrainActions(syncpoint, action_storage);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

void SyncpointManager::CancelWaits() {
    std::scoped_lock lk(guard);
    stopping.store(true, std::memory_order_release);
    {
        const auto cancel = [this](auto& storage) {
            for (auto& queue : storage) {
                if (!detached_actions && !queue.ordered_completion) continue;
                for (auto& entry : queue.actions) {
                    entry->state.store(RegisteredAction::State::Cancelled,
                                       std::memory_order_release);
                    entry->state.notify_all();
                }
                queue.actions.clear();
                queue.completed_values.clear();
            }
        };
        cancel(guest_action_storage);
        cancel(host_action_storage);
    }
    wait_guest_cv.notify_all();
    wait_host_cv.notify_all();
}

void SyncpointManager::PulseRaise(u32 syncpoint_id, u32 guest_value, u32 host_value) {
    const auto raise = [&](std::atomic<u32>& syncpoint, ActionQueue& action_storage,
                           u32 value) {
        if (HasReached(syncpoint.load(std::memory_order_acquire), value)) return;
        syncpoint.store(value, std::memory_order_release);
        std::erase_if(action_storage.completed_values, [value](u32 completed) {
            return HasReached(value, completed);
        });
        if (detached_actions || action_storage.ordered_completion) return;
        while (!action_storage.actions.empty() &&
               HasReached(value, action_storage.actions.front()->expected_value)) {
            auto entry = std::move(action_storage.actions.front());
            action_storage.actions.pop_front();
            RunAction(entry);
        }
    };
    std::unique_lock lk(guard);
    raise(syncpoints_guest.at(syncpoint_id), guest_action_storage[syncpoint_id], guest_value);
    raise(syncpoints_host.at(syncpoint_id), host_action_storage[syncpoint_id], host_value);
    wait_guest_cv.notify_all();
    wait_host_cv.notify_all();
    const bool guest_outside_lock = detached_actions || guest_action_storage[syncpoint_id].ordered_completion;
    const bool host_outside_lock = detached_actions || host_action_storage[syncpoint_id].ordered_completion;
    lk.unlock();
    if (guest_outside_lock) {
        DrainActions(syncpoints_guest[syncpoint_id], guest_action_storage[syncpoint_id]);
    }
    if (host_outside_lock) {
        DrainActions(syncpoints_host[syncpoint_id], host_action_storage[syncpoint_id]);
    }
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 ActionQueue& action_storage) {
    auto new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};

    VideoCore::FrameTrace::ScopedSpan lock_span{
        VideoCore::FrameTrace::Event::SyncpointLockLong, new_value};
    std::unique_lock lk(guard);
    lock_span.Finish();
    const bool outside_lock = detached_actions || action_storage.ordered_completion;
    if (!outside_lock) {
        while (!action_storage.actions.empty() &&
               HasReached(new_value, action_storage.actions.front()->expected_value)) {
            auto entry = std::move(action_storage.actions.front());
            action_storage.actions.pop_front();
            RunAction(entry);
        }
    }
    wait_cv.notify_all();
    lk.unlock();
    if (outside_lock) DrainActions(syncpoint, action_storage);
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() {
        return HasReached(syncpoint.load(std::memory_order_acquire), expected_value) ||
               stopping.load(std::memory_order_acquire);
    };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra
