#include "ocs_guards.h"

namespace htsim_ocs {

bool all_tokens_ready(const OcsDependencyTracker& dependencies,
                      const std::vector<std::uint64_t>& token_ids) {
    for (const std::uint64_t token_id : token_ids) {
        if (!dependencies.token_ready(token_id)) {
            return false;
        }
    }
    return true;
}

bool all_epoch_groups_complete(
    const OcsProgramEpochSpec& epoch,
    const std::vector<std::unique_ptr<OcsFlowGroupRuntime>>& groups) {
    for (const std::uint64_t group_id : epoch.flow_group_ids) {
        if (!groups[static_cast<std::size_t>(group_id)]->complete()) {
            return false;
        }
    }
    return true;
}

bool all_step_paths_ready(
    const OcsLogicalStepSpec& step, const OcsExecutionPlanV2& plan,
    const std::vector<std::unique_ptr<OcsPlaneRuntime>>& planes) {
    for (const std::uint64_t group_id : step.flow_group_ids) {
        const OcsFlowGroupSpec& group = plan.flow_group_by_id(group_id);
        const OcsProgramEpochRuntime& epoch =
            planes[static_cast<std::size_t>(group.plane_id)]->epoch_by_id(
                group.program_epoch_id);
        if (!epoch.path_ready()) {
            return false;
        }
    }
    return true;
}

}  // namespace htsim_ocs
