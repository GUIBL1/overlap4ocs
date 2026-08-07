#ifndef OVERLAP4OCS_HTSIM_OCS_RUN_SUMMARY_H
#define OVERLAP4OCS_HTSIM_OCS_RUN_SUMMARY_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ocs_flow.h"
#include "ocs_plane_dataplane.h"
#include "ocs_trace_collector.h"

namespace htsim_ocs {

struct OcsTokenRuntimeStats {
    std::uint64_t token_id;
    std::string token_type;
    std::optional<std::uint64_t> producer_id;
    std::string status;
    std::optional<std::uint64_t> ready_ps;
};

struct OcsStepRuntimeStats {
    std::uint64_t step_id;
    std::uint64_t expected_flow_group_count;
    std::uint64_t completed_flow_group_count;
    std::optional<std::uint64_t> first_flow_group_release_ps;
    std::optional<std::uint64_t> completion_ps;
    std::string status;
};

struct OcsFlowGroupRuntimeStats {
    std::uint64_t flow_group_id;
    std::uint64_t step_id;
    std::uint64_t plane_id;
    std::uint64_t program_epoch_id;
    std::uint64_t configuration_id;
    std::uint64_t physical_config_generation;
    std::vector<std::uint64_t> depends_on_token_ids;
    std::vector<std::uint64_t> flow_ids;
    std::optional<std::uint64_t> dependency_ready_ps;
    std::optional<std::uint64_t> release_ps;
    std::optional<std::uint64_t> completion_ps;
    std::uint64_t expected_flow_count;
    std::uint64_t completed_flow_count;
    std::uint64_t expected_payload_bytes;
    std::string status;
};

struct OcsEpochRuntimeStats {
    std::uint64_t program_epoch_id;
    std::string transition;
    std::uint64_t configuration_id;
    std::uint64_t physical_config_generation;
    std::vector<std::uint64_t> path_prep_not_before_token_ids;
    std::vector<std::uint64_t> flow_group_ids;
    std::optional<std::uint64_t> path_prep_start_ps;
    std::optional<std::uint64_t> path_ready_ps;
    std::optional<std::uint64_t> reconfiguration_start_ps;
    std::optional<std::uint64_t> reconfiguration_end_ps;
    std::optional<std::uint64_t> transfer_start_ps;
    std::optional<std::uint64_t> transfer_complete_ps;
    std::optional<std::uint64_t> drain_complete_ps;
    std::optional<std::uint64_t> close_ps;
    std::string status;
};

struct OcsPlaneRuntimeStats {
    std::uint64_t plane_id;
    std::string state;
    std::uint64_t program_cursor;
    std::optional<std::uint64_t> active_program_epoch_id;
    std::optional<std::uint64_t> active_configuration_id;
    std::optional<std::uint64_t> physical_config_generation;
    std::uint64_t serializer_backlog_flow_count;
    std::uint64_t serializer_backlog_bytes;
    std::uint64_t in_flight_transit_unit_count;
    std::vector<std::uint64_t> pending_path_prep_token_ids;
    std::vector<OcsEpochRuntimeStats> epochs;
};

struct OcsRuntimeTrafficSummary {
    std::uint64_t expected_flow_count;
    std::uint64_t completed_flow_count;
    std::uint64_t expected_flow_group_count;
    std::uint64_t completed_flow_group_count;
    std::uint64_t expected_payload_bytes;
    std::uint64_t sent_payload_bytes;
    std::uint64_t received_payload_bytes;
    std::uint64_t logical_packet_count;
    std::uint64_t simulated_transit_unit_count;
    std::uint64_t processed_event_count;
    std::uint64_t in_flight_transit_unit_count;
    std::vector<std::uint64_t> per_rank_sent_payload_bytes;
    std::vector<std::uint64_t> per_rank_received_payload_bytes;
    std::vector<std::uint64_t> per_plane_payload_bytes;
};

struct OcsBlockedState {
    std::vector<std::uint64_t> unfinished_token_ids;
    std::vector<std::uint64_t> unfinished_step_ids;
    std::vector<std::uint64_t> unfinished_flow_group_ids;
    std::vector<std::uint64_t> unfinished_flow_ids;
    std::optional<std::uint64_t> next_event_time_ps;
    std::vector<OcsPlaneRuntimeStats> planes;
};

struct OcsRunSummary {
    std::string status;
    std::string stop_reason;
    std::optional<std::string> error_code;
    std::string execution_mode;
    std::string dependency_mode;
    std::string path_preparation_policy;
    std::uint64_t simulation_stop_ps;
    std::optional<std::uint64_t> collective_complete_ps;
    OcsRuntimeTrafficSummary traffic;
    std::vector<OcsTokenRuntimeStats> tokens;
    std::vector<OcsStepRuntimeStats> steps;
    std::vector<OcsFlowGroupRuntimeStats> flow_groups;
    std::vector<OcsFlowStats> flows;
    std::vector<OcsPlaneRuntimeStats> planes;
    std::vector<OcsPlaneDataplaneStats> dataplane_planes;
    bool all_routes_exact;
    bool all_pools_returned;
    std::vector<OcsTraceEvent> trace;
    std::optional<OcsBlockedState> blocked_state;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_RUN_SUMMARY_H
