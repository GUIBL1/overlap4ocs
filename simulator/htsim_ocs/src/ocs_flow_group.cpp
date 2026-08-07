#include "ocs_flow_group.h"

#include <algorithm>
#include <utility>

#include "checked_arithmetic.h"
#include "ocs_dataplane_error.h"

namespace htsim_ocs {

OcsFlowGroupRuntime::OcsFlowGroupRuntime(
    const OcsFlowGroupSpec& spec,
    std::uint64_t physical_config_generation,
    std::uint64_t expected_payload_bytes)
    : spec_(spec),
      physical_config_generation_(physical_config_generation),
      expected_payload_bytes_(expected_payload_bytes),
      completed_flows_(spec.flow_ids.size(), false) {}

void OcsFlowGroupRuntime::mark_dependency_ready(std::uint64_t time_ps) {
    if (dependency_ready_ps_.has_value()) {
        throw OcsDataplaneError("dependency_violation",
                                "flow group became dependency-ready twice");
    }
    dependency_ready_ps_ = time_ps;
}

void OcsFlowGroupRuntime::mark_released(std::uint64_t time_ps) {
    if (!dependency_ready_ps_.has_value() || release_ps_.has_value() ||
        time_ps < *dependency_ready_ps_) {
        throw OcsDataplaneError("dependency_violation",
                                "flow group release violated dependency state");
    }
    release_ps_ = time_ps;
}

bool OcsFlowGroupRuntime::note_flow_complete(std::uint64_t flow_id,
                                             std::uint64_t time_ps) {
    if (!release_ps_.has_value() || completion_ps_.has_value()) {
        throw OcsDataplaneError("dependency_violation",
                                "flow completed outside a released group");
    }
    const auto found =
        std::lower_bound(spec_.flow_ids.begin(), spec_.flow_ids.end(), flow_id);
    if (found == spec_.flow_ids.end() || *found != flow_id) {
        throw OcsDataplaneError("dependency_violation",
                                "flow completion belongs to another group");
    }
    const std::size_t index =
        static_cast<std::size_t>(found - spec_.flow_ids.begin());
    if (completed_flows_[index]) {
        throw OcsDataplaneError("duplicate_flow_completion",
                                "flow completed twice in its group");
    }
    completed_flows_[index] = true;
    if (!checked_add_u64(completed_flow_count_, 1, completed_flow_count_)) {
        throw OcsDataplaneError("group_completion_overflow",
                                "group completion counter overflow");
    }
    if (completed_flow_count_ == spec_.flow_ids.size()) {
        completion_ps_ = time_ps;
        return true;
    }
    return false;
}

OcsFlowGroupRuntimeStats OcsFlowGroupRuntime::stats() const {
    std::string status = "pending";
    if (completion_ps_.has_value()) {
        status = "complete";
    } else if (release_ps_.has_value()) {
        status = "released";
    }
    return OcsFlowGroupRuntimeStats{
        spec_.flow_group_id,
        spec_.step_id,
        spec_.plane_id,
        spec_.program_epoch_id,
        spec_.configuration_id,
        physical_config_generation_,
        spec_.depends_on_token_ids,
        spec_.flow_ids,
        dependency_ready_ps_,
        release_ps_,
        completion_ps_,
        static_cast<std::uint64_t>(spec_.flow_ids.size()),
        completed_flow_count_,
        expected_payload_bytes_,
        std::move(status)};
}

OcsStepRuntime::OcsStepRuntime(const OcsLogicalStepSpec& spec)
    : spec_(spec), completed_groups_(spec.flow_group_ids.size(), false) {}

void OcsStepRuntime::note_group_release(std::uint64_t time_ps) {
    if (!first_release_ps_.has_value()) {
        first_release_ps_ = time_ps;
    }
}

bool OcsStepRuntime::note_group_complete(std::uint64_t group_id,
                                         std::uint64_t time_ps) {
    const auto found = std::lower_bound(spec_.flow_group_ids.begin(),
                                        spec_.flow_group_ids.end(), group_id);
    if (found == spec_.flow_group_ids.end() || *found != group_id) {
        throw OcsDataplaneError("dependency_violation",
                                "group completion belongs to another step");
    }
    const std::size_t index =
        static_cast<std::size_t>(found - spec_.flow_group_ids.begin());
    if (completed_groups_[index] || completion_ps_.has_value()) {
        throw OcsDataplaneError("duplicate_group_completion",
                                "group completed twice in its step");
    }
    completed_groups_[index] = true;
    if (!checked_add_u64(completed_group_count_, 1,
                         completed_group_count_)) {
        throw OcsDataplaneError("step_completion_overflow",
                                "step completion counter overflow");
    }
    if (completed_group_count_ == spec_.flow_group_ids.size()) {
        completion_ps_ = time_ps;
        return true;
    }
    return false;
}

OcsStepRuntimeStats OcsStepRuntime::stats() const {
    std::string status = "pending";
    if (completion_ps_.has_value()) {
        status = "complete";
    } else if (first_release_ps_.has_value()) {
        status = "active";
    }
    return OcsStepRuntimeStats{
        spec_.step_id,
        static_cast<std::uint64_t>(spec_.flow_group_ids.size()),
        completed_group_count_, first_release_ps_, completion_ps_,
        std::move(status)};
}

}  // namespace htsim_ocs
