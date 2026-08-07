#ifndef OVERLAP4OCS_HTSIM_OCS_TRACE_COLLECTOR_H
#define OVERLAP4OCS_HTSIM_OCS_TRACE_COLLECTOR_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace htsim_ocs {

struct OcsTraceContext {
    std::optional<std::uint64_t> plane_id;
    std::optional<std::uint64_t> program_epoch_id;
    std::optional<std::uint64_t> physical_config_generation;
    std::optional<std::uint64_t> configuration_id;
    std::optional<std::uint64_t> step_id;
    std::optional<std::uint64_t> flow_group_id;
    std::optional<std::uint64_t> flow_id;
    std::optional<std::uint64_t> token_id;
    std::optional<std::string> reason_code;
};

struct OcsTraceEvent {
    std::uint64_t event_index;
    std::uint64_t time_ps;
    std::string event_type;
    OcsTraceContext context;
};

class OcsTraceCollector final {
  public:
    void record(std::uint64_t time_ps, std::string event_type,
                OcsTraceContext context = {});

    const std::vector<OcsTraceEvent>& events() const noexcept {
        return events_;
    }

  private:
    std::vector<OcsTraceEvent> events_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_TRACE_COLLECTOR_H
