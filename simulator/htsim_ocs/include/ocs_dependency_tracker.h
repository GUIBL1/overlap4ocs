#ifndef OVERLAP4OCS_HTSIM_OCS_DEPENDENCY_TRACKER_H
#define OVERLAP4OCS_HTSIM_OCS_DEPENDENCY_TRACKER_H

#include <cstdint>
#include <optional>
#include <vector>

#include "ocs_execution_plan.h"
#include "ocs_run_summary.h"

namespace htsim_ocs {

class OcsDependencyTracker final {
  public:
    explicit OcsDependencyTracker(const OcsExecutionPlanV2& plan);

    std::vector<std::uint64_t> publish(std::uint64_t token_id,
                                       std::uint64_t time_ps);
    bool token_ready(std::uint64_t token_id) const;
    bool group_ready(std::uint64_t flow_group_id) const;
    std::uint64_t remaining_parent_count(std::uint64_t flow_group_id) const;
    std::vector<std::uint64_t> pending_token_ids() const;
    std::vector<OcsTokenRuntimeStats> stats() const;

  private:
    const OcsExecutionPlanV2& plan_;
    std::vector<bool> token_ready_;
    std::vector<std::optional<std::uint64_t>> token_ready_ps_;
    std::vector<std::uint64_t> remaining_parents_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_DEPENDENCY_TRACKER_H
