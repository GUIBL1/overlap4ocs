#include "ocs_dependency_tracker.h"

#include "ocs_dataplane_error.h"

namespace htsim_ocs {

OcsDependencyTracker::OcsDependencyTracker(const OcsExecutionPlanV2& plan)
    : plan_(plan),
      token_ready_(plan.readiness_tokens.size(), false),
      token_ready_ps_(plan.readiness_tokens.size()),
      remaining_parents_(plan.flow_groups.size(), 0) {
    for (const OcsFlowGroupSpec& group : plan.flow_groups) {
        remaining_parents_[static_cast<std::size_t>(group.flow_group_id)] =
            static_cast<std::uint64_t>(group.depends_on_token_ids.size());
    }
}

std::vector<std::uint64_t> OcsDependencyTracker::publish(
    std::uint64_t token_id, std::uint64_t time_ps) {
    if (token_id >= token_ready_.size()) {
        throw OcsDataplaneError("dependency_violation",
                                "published token ID is outside the plan");
    }
    if (token_ready_[static_cast<std::size_t>(token_id)]) {
        throw OcsDataplaneError("dependency_violation",
                                "readiness token was published twice");
    }
    token_ready_[static_cast<std::size_t>(token_id)] = true;
    token_ready_ps_[static_cast<std::size_t>(token_id)] = time_ps;

    std::vector<std::uint64_t> newly_ready;
    for (const std::uint64_t group_id :
         plan_.traffic_inventory.dependency_children_by_token
             [static_cast<std::size_t>(token_id)]) {
        std::uint64_t& remaining =
            remaining_parents_[static_cast<std::size_t>(group_id)];
        if (remaining == 0) {
            throw OcsDataplaneError("dependency_counter_underflow",
                                    "dependency parent counter underflow");
        }
        --remaining;
        if (remaining == 0) {
            newly_ready.push_back(group_id);
        }
    }
    return newly_ready;
}

bool OcsDependencyTracker::token_ready(std::uint64_t token_id) const {
    if (token_id >= token_ready_.size()) {
        throw OcsDataplaneError("dependency_violation",
                                "token ID is outside the plan");
    }
    return token_ready_[static_cast<std::size_t>(token_id)];
}

bool OcsDependencyTracker::group_ready(std::uint64_t flow_group_id) const {
    return remaining_parent_count(flow_group_id) == 0;
}

std::uint64_t OcsDependencyTracker::remaining_parent_count(
    std::uint64_t flow_group_id) const {
    if (flow_group_id >= remaining_parents_.size()) {
        throw OcsDataplaneError("dependency_violation",
                                "flow group ID is outside the plan");
    }
    return remaining_parents_[static_cast<std::size_t>(flow_group_id)];
}

std::vector<std::uint64_t> OcsDependencyTracker::pending_token_ids() const {
    std::vector<std::uint64_t> result;
    for (std::size_t token_id = 0; token_id < token_ready_.size(); ++token_id) {
        if (!token_ready_[token_id]) {
            result.push_back(static_cast<std::uint64_t>(token_id));
        }
    }
    return result;
}

std::vector<OcsTokenRuntimeStats> OcsDependencyTracker::stats() const {
    std::vector<OcsTokenRuntimeStats> result;
    result.reserve(plan_.readiness_tokens.size());
    for (const OcsReadinessToken& token : plan_.readiness_tokens) {
        const std::size_t index = static_cast<std::size_t>(token.token_id);
        result.push_back(OcsTokenRuntimeStats{
            token.token_id, token.token_type, token.producer_id,
            token_ready_[index] ? "ready" : "pending", token_ready_ps_[index]});
    }
    return result;
}

}  // namespace htsim_ocs
