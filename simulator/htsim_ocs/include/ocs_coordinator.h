#ifndef OVERLAP4OCS_HTSIM_OCS_COORDINATOR_H
#define OVERLAP4OCS_HTSIM_OCS_COORDINATOR_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ocs_dependency_tracker.h"
#include "ocs_execution_mode.h"
#include "ocs_flow_group.h"
#include "ocs_plane_runtime.h"
#include "ocs_run_summary.h"
#include "ocs_topology.h"
#include "ocs_trace_collector.h"

class EventList;

namespace htsim_ocs {

struct OcsRuntimeFaultInjection {
    std::optional<std::uint64_t> suppress_flow_completion_id;
    std::optional<std::uint64_t> duplicate_flow_completion_id;
};

class OcsCoordinator final {
  public:
    OcsCoordinator(EventList& event_list,
                   std::shared_ptr<const OcsExecutionPlanV2> plan,
                   OcsExecutionMode mode, OcsCapacityProof capacity_proof,
                   OcsRuntimeFaultInjection fault_injection = {});

    OcsRunSummary run();

  private:
    void initialize();
    void publish_token(std::uint64_t token_id, std::uint64_t time_ps);
    void on_flow_last_sent(const OcsFlowStats& stats);
    void on_flow_complete(const OcsFlowStats& stats);
    void process_flow_complete(const OcsFlowStats& stats);
    void on_reconfiguration_complete(std::uint64_t plane_id,
                                     std::uint64_t program_epoch_id);

    void pump();
    bool close_completed_epochs();
    bool prepare_permitted_paths();
    bool update_lockstep_gates();
    bool release_permitted_groups();
    void begin_current_epoch(OcsPlaneRuntime& plane);
    bool run_complete() const;
    void verify_success() const;

    OcsRunSummary make_summary(
        std::string status, std::string stop_reason,
        std::optional<std::string> error_code,
        std::optional<std::uint64_t> next_event_time_ps);
    std::vector<OcsPlaneRuntimeStats> plane_stats() const;
    OcsBlockedState blocked_state(
        std::optional<std::uint64_t> next_event_time_ps) const;
    void cleanup_after_failure();

    EventList& event_list_;
    std::shared_ptr<const OcsExecutionPlanV2> plan_;
    OcsExecutionMode mode_;
    OcsTopology topology_;
    OcsDependencyTracker dependencies_;
    std::vector<std::unique_ptr<OcsFlowGroupRuntime>> groups_;
    std::vector<std::unique_ptr<OcsStepRuntime>> steps_;
    std::vector<std::unique_ptr<OcsPlaneRuntime>> planes_;
    std::vector<bool> lockstep_gate_ready_;
    OcsTraceCollector trace_;
    OcsRuntimeFaultInjection fault_injection_;
    bool duplicate_callback_injected_ = false;
    std::uint64_t completed_group_count_ = 0;
};

OcsRunSummary run_ocs_runtime(
    std::shared_ptr<const OcsExecutionPlanV2> plan,
    OcsRuntimeFaultInjection fault_injection = {});

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_COORDINATOR_H
