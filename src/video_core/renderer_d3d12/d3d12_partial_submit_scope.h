#pragma once

#include <cstdint>

namespace D3D12 {

class Scheduler;

class PartialSubmitScope {
public:
    PartialSubmitScope(Scheduler& scheduler_, std::uint32_t& pending_draws_)
        : scheduler{scheduler_}, pending_draws{pending_draws_} {}
    ~PartialSubmitScope() noexcept(false);
    PartialSubmitScope(const PartialSubmitScope&) = delete;
    PartialSubmitScope& operator=(const PartialSubmitScope&) = delete;
    void Recorded();

private:
    Scheduler& scheduler;
    std::uint32_t& pending_draws;
    std::uint64_t recorded_tick{};
};

}
