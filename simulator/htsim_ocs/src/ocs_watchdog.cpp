#include "ocs_watchdog.h"

#include "eventlist.h"

namespace htsim_ocs {

OcsWatchdogDecision OcsWatchdog::evaluate(const OcsRunLimits& limits) {
    std::uint64_t next_time = 0;
    if (!EventList::nextEventTime(next_time)) {
        return {false, "deadlock", "no_progress", std::nullopt};
    }
    if (EventList::processedEventCount() >= limits.max_events) {
        return {false, "max_event_count", "max_event_count_reached",
                next_time};
    }
    if (next_time > limits.max_sim_time_ps) {
        return {false, "max_simulation_time",
                "max_simulation_time_reached", next_time};
    }
    return {true, "", "", next_time};
}

}  // namespace htsim_ocs
