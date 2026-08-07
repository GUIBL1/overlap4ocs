#ifndef OVERLAP4OCS_HTSIM_OCS_GUARDS_H
#define OVERLAP4OCS_HTSIM_OCS_GUARDS_H

#include <cstdint>
#include <memory>
#include <vector>

#include "ocs_dependency_tracker.h"
#include "ocs_execution_plan.h"
#include "ocs_flow_group.h"
#include "ocs_plane_runtime.h"

namespace htsim_ocs {

bool all_tokens_ready(const OcsDependencyTracker& dependencies,
                      const std::vector<std::uint64_t>& token_ids);
bool all_epoch_groups_complete(
    const OcsProgramEpochSpec& epoch,
    const std::vector<std::unique_ptr<OcsFlowGroupRuntime>>& groups);
bool all_step_paths_ready(
    const OcsLogicalStepSpec& step, const OcsExecutionPlanV2& plan,
    const std::vector<std::unique_ptr<OcsPlaneRuntime>>& planes);

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_GUARDS_H
