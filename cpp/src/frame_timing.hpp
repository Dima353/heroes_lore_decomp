#pragma once
#include <cstdint>

// Advance by the requested period, retaining the fractional render-frame remainder.
// A long load/pause resets the clock instead of replaying a backlog of game ticks.
inline std::uint32_t advance_logic_tick(std::uint32_t previous,std::uint32_t now,std::uint32_t period) {
    const auto elapsed=now-previous;
    return elapsed<period || elapsed>period*4?now:previous+period;
}
