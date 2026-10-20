#pragma once

#include <algorithm>
#include <cstdint>

namespace D3D12 {

class ReadbackReserve {
public:
    static constexpr std::uint64_t Budget = 8ULL << 20;
    static constexpr std::uint64_t MaxEntry = 2ULL << 20;
    static constexpr std::uint64_t MinEntry = 64ULL << 10;
    static constexpr std::uint64_t MaxAge = 120;
    static constexpr std::uint64_t NewEntryAge = 8;

    void Configure(std::uint64_t free_bytes, bool known) {
        target = known ? (std::min)(Budget, free_bytes / 8) & ~(MinEntry - 1) : Budget;
    }

    std::uint64_t Retain(std::uint64_t capacity) {
        if (capacity < MinEntry || capacity > MaxEntry || retained > target)
            return 0;
        const auto charge = (capacity + MinEntry - 1) & ~(MinEntry - 1);
        if (charge > target - retained)
            return 0;
        retained += charge;
        return charge;
    }

    void Release(std::uint64_t charge) {
        retained -= charge;
    }

    static bool Keep(std::uint64_t last_used, std::uint64_t frame, bool forced,
                     std::uint32_t uses = 2) {
        return !forced && frame >= last_used &&
               frame - last_used <= (uses > 1 ? MaxAge : NewEntryAge);
    }

    std::uint64_t Bytes() const {
        return retained;
    }
    std::uint64_t Target() const {
        return target;
    }

private:
    std::uint64_t retained{};
    std::uint64_t target{Budget};
};

} // namespace D3D12
