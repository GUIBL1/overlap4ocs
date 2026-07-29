#ifndef OVERLAP4OCS_HTSIM_OCS_EXECUTION_PLAN_H
#define OVERLAP4OCS_HTSIM_OCS_EXECUTION_PLAN_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace htsim_ocs {

struct OcsUnits {
    std::string data;
    std::string rate;
    std::string time;
};

struct OcsWorkloadSpec {
    std::string collective_id;
    std::string algorithm_id;
    std::string algorithm_semantics_version;
    std::string collective_semantics_version;
    std::uint64_t rank_count;
    std::uint64_t message_bytes_per_rank;
};

struct OcsTopologySpec {
    std::uint64_t node_count;
    std::uint64_t plane_count;
    std::uint64_t per_plane_bps;
    std::uint64_t data_latency_ps;
    std::uint64_t reconfiguration_delay_ps;
    std::string duplex;
    std::string initial_configuration_policy;
};

struct OcsTransportSpec {
    std::string mode;
    std::string wire_model;
    std::string packetization;
    std::uint64_t mtu_bytes;
    std::string loss_mode;
    std::string ack_mode;
    std::string completion_semantics;
};

struct OcsRunLimits {
    std::uint64_t seed;
    std::uint64_t max_sim_time_ps;
    std::uint64_t max_events;
};

struct OcsConfiguration {
    std::uint64_t configuration_id;
    std::vector<std::uint64_t> permutation;
};

struct OcsLogicalSegment {
    std::uint64_t segment_id;
    std::uint64_t buffer_offset_bytes;
    std::uint64_t length_bytes;
    std::vector<std::uint64_t> origin_rank_ids;
    std::vector<std::uint64_t> final_destination_rank_ids;
};

struct OcsReadinessToken {
    std::uint64_t token_id;
    std::string token_type;
    std::optional<std::uint64_t> producer_id;
};

struct OcsLogicalStepSpec {
    std::uint64_t step_id;
    std::string phase;
    std::vector<std::uint64_t> flow_group_ids;
    std::uint64_t completion_token_id;
};

struct OcsFlowGroupSpec {
    std::uint64_t flow_group_id;
    std::uint64_t step_id;
    std::uint64_t plane_id;
    std::uint64_t program_epoch_id;
    std::uint64_t configuration_id;
    std::vector<std::uint64_t> depends_on_token_ids;
    std::uint64_t completion_token_id;
    std::vector<std::uint64_t> flow_ids;
};

struct OcsSegmentSlice {
    std::uint64_t segment_id;
    std::uint64_t segment_offset_bytes;
    std::uint64_t length_bytes;
};

struct OcsFlowSpec {
    std::uint64_t flow_id;
    std::uint64_t flow_group_id;
    std::uint64_t src_rank;
    std::uint64_t dst_rank;
    std::uint64_t payload_bytes;
    std::vector<OcsSegmentSlice> segment_slices;
};

struct OcsProgramEpochSpec {
    std::uint64_t program_epoch_id;
    std::string transition;
    std::uint64_t configuration_id;
    std::vector<std::uint64_t> path_prep_not_before_token_ids;
    std::vector<std::uint64_t> flow_group_ids;
};

struct OcsPlaneProgram {
    std::uint64_t plane_id;
    std::vector<OcsProgramEpochSpec> epochs;
};

struct OcsNominalScheduleEntry {
    std::uint64_t flow_group_id;
    std::uint64_t planned_release_ps;
    std::uint64_t planned_complete_ps;
};

struct OcsPlannerCertificate {
    std::string planner_name;
    std::string planner_version;
    std::optional<std::string> solver_name;
    std::string solver_status;
    std::optional<std::uint64_t> objective_ps;
    std::optional<std::uint64_t> bound_ps;
    std::optional<std::uint64_t> relative_gap_ppm;
    std::optional<std::string> decision_sha256;
    std::vector<OcsNominalScheduleEntry> nominal_schedule;
    std::string integer_lowering_rule;
    std::string integer_lowering_version;
};

struct OcsProvenance {
    std::string source_kind;
    std::optional<std::string> overlap4ocs_git_sha;
    std::optional<std::string> htsim_upstream_git_sha;
    std::optional<std::string> htsim_local_patchset_sha256;
    std::optional<std::string> instance_file_sha256;
    std::optional<std::string> program_file_sha256;
    std::optional<std::string> collective_ir_sha256;
    std::optional<std::string> plan_build_context_sha256;
};

struct DerivedTrafficInventory {
    std::uint64_t expected_flow_count;
    std::uint64_t expected_flow_group_count;
    std::uint64_t expected_payload_bytes;
    std::vector<std::uint64_t> per_rank_sent_payload_bytes;
    std::vector<std::uint64_t> per_rank_received_payload_bytes;
    std::vector<std::uint64_t> per_plane_payload_bytes;
    std::vector<std::vector<std::uint64_t>> dependency_children_by_token;
};

struct OcsExecutionPlanV2 {
    std::string schema_version;
    std::string case_id;
    std::string strategy;
    OcsUnits units;
    OcsWorkloadSpec workload;
    OcsTopologySpec topology;
    OcsTransportSpec transport;
    std::string execution_mode;
    OcsRunLimits run_limits;
    std::string dependency_mode;
    std::string path_preparation_policy;
    std::vector<OcsConfiguration> configurations;
    std::vector<OcsLogicalSegment> logical_segments;
    std::vector<OcsReadinessToken> readiness_tokens;
    std::vector<OcsLogicalStepSpec> steps;
    std::vector<OcsFlowGroupSpec> flow_groups;
    std::vector<OcsFlowSpec> flows;
    std::vector<OcsPlaneProgram> plane_programs;
    OcsPlannerCertificate planner_certificate;
    OcsProvenance provenance;
    DerivedTrafficInventory traffic_inventory;

    const OcsConfiguration& configuration_by_id(std::uint64_t id) const;
    const OcsLogicalStepSpec& step_by_id(std::uint64_t id) const;
    const OcsFlowGroupSpec& flow_group_by_id(std::uint64_t id) const;
    const OcsFlowSpec& flow_by_id(std::uint64_t id) const;
    const OcsPlaneProgram& plane_program_by_id(std::uint64_t id) const;
};

struct OcsValidationSummary {
    std::string plan_file_sha256;
    std::uint64_t raw_file_bytes;
    std::uint64_t conservative_result_upper_bound_bytes;
};

struct ParsedExecutionPlan {
    std::shared_ptr<const OcsExecutionPlanV2> plan;
    OcsValidationSummary summary;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_EXECUTION_PLAN_H
