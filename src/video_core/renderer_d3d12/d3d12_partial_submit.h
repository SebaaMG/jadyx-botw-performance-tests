#pragma once

#include <cstdint>

namespace D3D12 {

class PartialSubmitPolicy {
public:
    static constexpr std::uint32_t CheckInterval = 64;
    static constexpr std::uint32_t MinWork = 128;
    static constexpr std::uint32_t MaxPerFrame = 3;
    static constexpr std::uint64_t BudgetUs = 4000;
    static constexpr std::uint64_t DrainedQueueBudgetUs = 2000;
    static constexpr std::uint64_t MaxInFlight = 2;

    explicit PartialSubmitPolicy(bool continuous_ = false) : continuous{continuous_} {}

    bool RecordWork() {
        if (!continuous && partial_count >= MaxPerFrame) return false;
        ++pending_work;
        return pending_work == 1 ||
               (pending_work >= MinWork && pending_work % CheckInterval == 0);
    }

    bool Due(std::uint64_t now_us, std::uint64_t outstanding = 0) {
        if (!started) {
            started = true;
            start_us = now_us;
            return false;
        }
        const auto budget = continuous && outstanding == 0 ? DrainedQueueBudgetUs : BudgetUs;
        return (continuous || partial_count < MaxPerFrame) && outstanding < MaxInFlight &&
               pending_work >= MinWork && now_us >= start_us && now_us - start_us >= budget;
    }

    void Submitted() {
        pending_work = 0;
        started = false;
    }

    void PartiallySubmitted() {
        if (!continuous) ++partial_count;
        Submitted();
    }

    void BeginFrame() { partial_count = 0; }
    std::uint32_t PendingWork() const { return pending_work; }

private:
    const bool continuous;
    std::uint64_t start_us{};
    std::uint32_t pending_work{};
    std::uint32_t partial_count{};
    bool started{};
};

}
