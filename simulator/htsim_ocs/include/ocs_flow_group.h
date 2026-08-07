#ifndef OVERLAP4OCS_HTSIM_OCS_FLOW_GROUP_H
#define OVERLAP4OCS_HTSIM_OCS_FLOW_GROUP_H

#include <cstdint>
#include <optional>
#include <vector>

#include "ocs_execution_plan.h"
#include "ocs_run_summary.h"

namespace htsim_ocs {

class OcsFlowGroupRuntime final {
  public:
    OcsFlowGroupRuntime(const OcsFlowGroupSpec& spec,
                        std::uint64_t physical_config_generation,
                        std::uint64_t expected_payload_bytes);

    const OcsFlowGroupSpec& spec() const noexcept { return spec_; }
    void mark_dependency_ready(std::uint64_t time_ps);
    void mark_released(std::uint64_t time_ps);
    bool note_flow_complete(std::uint64_t flow_id, std::uint64_t time_ps);

    bool dependency_ready() const noexcept {
        return dependency_ready_ps_.has_value();
    }
    bool released() const noexcept { return release_ps_.has_value(); }
    bool complete() const noexcept { return completion_ps_.has_value(); }
    const std::optional<std::uint64_t>& completion_ps() const noexcept {
        return completion_ps_;
    }
    OcsFlowGroupRuntimeStats stats() const;

  private:
    const OcsFlowGroupSpec& spec_;
    std::uint64_t physical_config_generation_;
    std::uint64_t expected_payload_bytes_;
    std::vector<bool> completed_flows_;
    std::uint64_t completed_flow_count_ = 0;
    std::optional<std::uint64_t> dependency_ready_ps_;
    std::optional<std::uint64_t> release_ps_;
    std::optional<std::uint64_t> completion_ps_;
};

class OcsStepRuntime final {
  public:
    explicit OcsStepRuntime(const OcsLogicalStepSpec& spec);

    const OcsLogicalStepSpec& spec() const noexcept { return spec_; }
    void note_group_release(std::uint64_t time_ps);
    bool note_group_complete(std::uint64_t group_id, std::uint64_t time_ps);
    bool complete() const noexcept { return completion_ps_.has_value(); }
    OcsStepRuntimeStats stats() const;

  private:
    const OcsLogicalStepSpec& spec_;
    std::vector<bool> completed_groups_;
    std::uint64_t completed_group_count_ = 0;
    std::optional<std::uint64_t> first_release_ps_;
    std::optional<std::uint64_t> completion_ps_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_FLOW_GROUP_H
