#include "ocs_trace_collector.h"

#include <limits>
#include <utility>

#include "ocs_dataplane_error.h"

namespace htsim_ocs {

void OcsTraceCollector::record(std::uint64_t time_ps, std::string event_type,
                               OcsTraceContext context) {
    if (event_type.empty()) {
        throw OcsDataplaneError("trace_event_type_empty",
                                "trace event type cannot be empty");
    }
    if (!events_.empty() && time_ps < events_.back().time_ps) {
        throw OcsDataplaneError("trace_time_regression",
                                "trace time moved backwards");
    }
    if (events_.size() == std::numeric_limits<std::uint64_t>::max()) {
        throw OcsDataplaneError("trace_event_index_overflow",
                                "trace event index exceeds uint64");
    }
    events_.push_back(OcsTraceEvent{
        static_cast<std::uint64_t>(events_.size()), time_ps,
        std::move(event_type), std::move(context)});
}

}  // namespace htsim_ocs
