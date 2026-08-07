#ifndef OVERLAP4OCS_HTSIM_OCS_WATCHDOG_H
#define OVERLAP4OCS_HTSIM_OCS_WATCHDOG_H

#include <cstdint>
#include <optional>
#include <string>

#include "ocs_execution_plan.h"

namespace htsim_ocs {

struct OcsWatchdogDecision {
    bool dispatch_next_event;
    std::string stop_reason;
    std::string error_code;
    std::optional<std::uint64_t> next_event_time_ps;
};

class OcsWatchdog final {
  public:
    static OcsWatchdogDecision evaluate(const OcsRunLimits& limits);
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_WATCHDOG_H
