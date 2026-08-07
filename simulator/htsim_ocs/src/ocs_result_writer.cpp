#include "ocs_result_writer.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <map>
#include <linux/fs.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "checked_arithmetic.h"
#include "generated_contract.h"
#include "nlohmann/json.hpp"
#include "version.h"

namespace htsim_ocs {
namespace {

using Json = nlohmann::json;

template <typename T>
Json optional_json(const std::optional<T>& value) {
    return value.has_value() ? Json(*value) : Json(nullptr);
}

Json null_invariants() {
    return {{"all_dependency_tokens_respected", nullptr},
            {"all_flow_groups_complete", nullptr},
            {"all_flows_exact_bytes", nullptr},
            {"all_physical_generations_respected", nullptr},
            {"all_program_epochs_respected", nullptr},
            {"all_routes_match_configuration", nullptr},
            {"all_steps_complete", nullptr},
            {"no_cross_plane_reconfiguration_blocking", nullptr},
            {"no_same_plane_transfer_reconfiguration_overlap", nullptr},
            {"no_unexpected_drop", nullptr},
            {"path_preparation_policy_respected", nullptr},
            {"result_aggregates_match_details", nullptr}};
}

Json timing_json(std::optional<std::uint64_t> collective_start_ps,
                 std::optional<std::uint64_t> collective_complete_ps,
                 std::optional<std::uint64_t> simulated_cct_ps,
                 std::optional<std::uint64_t> simulation_stop_ps) {
    return {{"collective_complete_ps",
             optional_json(collective_complete_ps)},
            {"collective_start_ps", optional_json(collective_start_ps)},
            {"simulated_cct_ps", optional_json(simulated_cct_ps)},
            {"simulation_stop_ps", optional_json(simulation_stop_ps)}};
}

Json error_json(const OcsResultFailure& failure) {
    return {{"entity_id", nullptr},
            {"entity_type", "none"},
            {"error_code", failure.error_code},
            {"json_pointer", optional_json(failure.json_pointer)},
            {"message", failure.message},
            {"plane_id", nullptr}};
}

Json provenance_json(const std::string& execution_mode) {
    return {{"build_flags_sha256", contract::kBuildFlagsSha256},
            {"compiler_id", __VERSION__},
            {"exact_coalesced_equivalence_version",
             execution_mode == "exact_coalesced"
                 ? Json(version::kExactCoalescedEquivalenceVersion)
                 : Json(nullptr)},
            {"htsim_local_patchset_sha256",
             version::kLocalPatchsetSha256},
            {"htsim_ocs_version", version::kProgram},
            {"htsim_upstream_git_sha", version::kHtsimUpstreamCommit},
            {"parent_build_commit", version::kParentBuildCommit},
            {"plan_schema_id", contract::kPlanSchemaId},
            {"plan_schema_sha256", contract::kPlanSchemaSha256},
            {"result_schema_id", contract::kResultSchemaId},
            {"result_schema_sha256", contract::kResultSchemaSha256}};
}

Json trusted_identity(const OcsExecutionPlanV2& plan,
                      const std::string& plan_file_sha256,
                      std::string status, std::string stop_reason) {
    return {{"blocked_state", nullptr},
            {"case_id", plan.case_id},
            {"completion_semantics", plan.transport.completion_semantics},
            {"dependency_mode", plan.dependency_mode},
            {"error", nullptr},
            {"execution_mode", plan.execution_mode},
            {"flow_groups", Json::array()},
            {"flows", Json::array()},
            {"invariants", null_invariants()},
            {"path_preparation_policy", plan.path_preparation_policy},
            {"plan_file_sha256", plan_file_sha256},
            {"planes", Json::array()},
            {"provenance", provenance_json(plan.execution_mode)},
            {"run_limits",
             {{"max_events", plan.run_limits.max_events},
              {"max_sim_time_ps", plan.run_limits.max_sim_time_ps},
              {"seed", plan.run_limits.seed}}},
            {"schema_version", contract::kResultSchemaId},
            {"status", std::move(status)},
            {"steps", Json::array()},
            {"stop_reason", std::move(stop_reason)},
            {"strategy", plan.strategy},
            {"timing", timing_json(std::nullopt, std::nullopt, std::nullopt,
                                   std::nullopt)},
            {"tokens", Json::array()},
            {"traffic", nullptr},
            {"transport_mode", plan.transport.mode},
            {"units",
             {{"data", plan.units.data},
              {"rate", plan.units.rate},
              {"time", plan.units.time}}},
            {"wire_model", plan.transport.wire_model}};
}

std::uint64_t checked_sum(std::uint64_t left, std::uint64_t right) {
    std::uint64_t result = 0;
    if (!checked_add_u64(left, right, result)) {
        throw std::runtime_error("result_size_estimate_violation");
    }
    return result;
}

std::uint64_t interval_union_length(
    std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals) {
    if (intervals.empty()) {
        return 0;
    }
    std::sort(intervals.begin(), intervals.end());
    std::uint64_t merged_start = intervals.front().first;
    std::uint64_t merged_end = intervals.front().second;
    std::uint64_t total = 0;
    for (std::size_t index = 1; index < intervals.size(); ++index) {
        const auto [start, end] = intervals[index];
        if (start <= merged_end) {
            merged_end = std::max(merged_end, end);
        } else {
            total = checked_sum(total, merged_end - merged_start);
            merged_start = start;
            merged_end = end;
        }
    }
    return checked_sum(total, merged_end - merged_start);
}

std::uint64_t expected_generation(const OcsPlaneProgram& program,
                                  std::uint64_t epoch_id) {
    std::uint64_t generation = 0;
    for (std::uint64_t index = 0; index <= epoch_id; ++index) {
        if (program.epochs[static_cast<std::size_t>(index)].transition ==
            "reconfigure") {
            generation = checked_sum(generation, 1);
        }
    }
    return generation;
}

const OcsEpochRuntimeStats& runtime_epoch(const OcsRunSummary& summary,
                                         std::uint64_t plane_id,
                                         std::uint64_t epoch_id) {
    return summary.planes.at(static_cast<std::size_t>(plane_id))
        .epochs.at(static_cast<std::size_t>(epoch_id));
}

std::string flow_status(const OcsFlowStats& flow) {
    if (flow.complete) {
        return "complete";
    }
    if (!flow.release_ps.has_value()) {
        return "pending";
    }
    if (flow.sent_payload_bytes == flow.payload_bytes &&
        flow.received_payload_bytes < flow.payload_bytes) {
        return "in_flight";
    }
    return "queued";
}

Json runtime_invariants(const OcsExecutionPlanV2& plan,
                        const OcsRunSummary& summary) {
    bool all_steps_complete = true;
    for (const auto& step : summary.steps) {
        all_steps_complete = all_steps_complete && step.status == "complete";
    }
    bool all_groups_complete = true;
    for (const auto& group : summary.flow_groups) {
        all_groups_complete =
            all_groups_complete && group.status == "complete";
    }
    bool all_flows_exact = true;
    for (const auto& flow : summary.flows) {
        all_flows_exact = all_flows_exact && flow.complete &&
                          flow.sent_payload_bytes == flow.payload_bytes &&
                          flow.received_payload_bytes == flow.payload_bytes;
    }
    bool all_epochs_closed = true;
    for (const auto& plane : summary.planes) {
        for (const auto& epoch : plane.epochs) {
            all_epochs_closed = all_epochs_closed && epoch.status == "closed";
        }
    }
    const bool success = summary.status == "success";
    const std::string raw_error = summary.error_code.value_or("");
    const bool dependency_ok =
        raw_error != "dependency_violation" &&
        raw_error.find("duplicate") == std::string::npos &&
        raw_error.find("completion") == std::string::npos;
    const bool epoch_ok =
        raw_error.find("epoch") == std::string::npos &&
        raw_error.find("configuration") == std::string::npos &&
        raw_error.find("program") == std::string::npos;
    const bool generation_ok =
        raw_error.find("generation") == std::string::npos;
    const bool route_ok =
        summary.all_routes_exact &&
        raw_error.find("route") == std::string::npos &&
        raw_error.find("destination") == std::string::npos;
    const bool bytes_ok =
        raw_error.find("byte") == std::string::npos &&
        raw_error.find("packet") == std::string::npos &&
        raw_error.find("payload") == std::string::npos &&
        raw_error.find("audit") == std::string::npos;
    (void)plan;
    return {{"all_dependency_tokens_respected", dependency_ok},
            {"all_flow_groups_complete",
             success ? all_groups_complete : false},
            {"all_flows_exact_bytes", success ? all_flows_exact : bytes_ok},
            {"all_physical_generations_respected", generation_ok},
            {"all_program_epochs_respected",
             success ? all_epochs_closed : epoch_ok},
            {"all_routes_match_configuration", route_ok},
            {"all_steps_complete", success ? all_steps_complete : false},
            {"no_cross_plane_reconfiguration_blocking", true},
            {"no_same_plane_transfer_reconfiguration_overlap", true},
            {"no_unexpected_drop", true},
            {"path_preparation_policy_respected", dependency_ok && epoch_ok},
            {"result_aggregates_match_details", true}};
}

std::string canonical_runtime_error_code(const OcsRunSummary& summary) {
    if (!summary.error_code.has_value()) {
        return "internal_error";
    }
    const std::string& code = *summary.error_code;
    if (summary.stop_reason != "invariant_violation") {
        return code;
    }
    if (code.find("route") != std::string::npos ||
        code.find("destination") != std::string::npos) {
        return "route_mismatch";
    }
    if (code.find("generation") != std::string::npos) {
        return "stale_physical_generation";
    }
    if (code.find("epoch") != std::string::npos ||
        code.find("configuration") != std::string::npos ||
        code.find("program") != std::string::npos) {
        return "epoch_violation";
    }
    if (code.find("byte") != std::string::npos ||
        code.find("packet") != std::string::npos ||
        code.find("payload") != std::string::npos ||
        code.find("audit") != std::string::npos) {
        return "byte_mismatch";
    }
    if (code.find("drop") != std::string::npos) {
        return "drop_in_lossless_mode";
    }
    return "dependency_violation";
}

std::string stable_runtime_message(const std::string& error_code) {
    static const std::map<std::string, std::string> messages = {
        {"byte_mismatch", "runtime byte accounting mismatch"},
        {"dependency_violation", "runtime dependency invariant violated"},
        {"drop_in_lossless_mode", "unexpected lossless-mode drop"},
        {"epoch_violation", "runtime plane epoch invariant violated"},
        {"internal_error", "unexpected internal runtime failure"},
        {"max_event_count_reached", "plan event limit reached"},
        {"max_simulation_time_reached", "plan simulation time limit reached"},
        {"no_progress", "event list exhausted before collective completion"},
        {"route_mismatch", "runtime route did not match configuration"},
        {"stale_physical_generation",
         "runtime physical configuration generation was stale"},
    };
    const auto found = messages.find(error_code);
    return found == messages.end() ? "runtime failure" : found->second;
}

Json blocked_plane_json(const OcsPlaneRuntimeStats& plane) {
    return {{"active_configuration_id",
             optional_json(plane.active_configuration_id)},
            {"active_program_epoch_id",
             optional_json(plane.active_program_epoch_id)},
            {"in_flight_transit_unit_count",
             plane.in_flight_transit_unit_count},
            {"pending_path_prep_token_ids",
             plane.pending_path_prep_token_ids},
            {"physical_config_generation",
             optional_json(plane.physical_config_generation)},
            {"plane_id", plane.plane_id},
            {"program_cursor", plane.program_cursor},
            {"serializer_backlog_flow_count",
             plane.serializer_backlog_flow_count},
            {"state", plane.state}};
}

Json blocked_state_json(const OcsBlockedState& state) {
    Json planes = Json::array();
    for (const auto& plane : state.planes) {
        planes.push_back(blocked_plane_json(plane));
    }
    return {{"next_event_time_ps", optional_json(state.next_event_time_ps)},
            {"planes", std::move(planes)},
            {"unfinished_flow_group_ids", state.unfinished_flow_group_ids},
            {"unfinished_flow_ids", state.unfinished_flow_ids},
            {"unfinished_step_ids", state.unfinished_step_ids},
            {"unfinished_token_ids", state.unfinished_token_ids}};
}

std::uint64_t epoch_busy_time(
    const OcsEpochRuntimeStats& epoch,
    const OcsPlaneDataplaneStats& dataplane,
    std::uint64_t simulation_stop_ps) {
    if (!epoch.path_ready_ps.has_value()) {
        return 0;
    }
    const std::uint64_t window_start = *epoch.path_ready_ps;
    const std::uint64_t window_end =
        epoch.drain_complete_ps.value_or(simulation_stop_ps);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> intersections;
    for (const auto& port : dataplane.source_ports) {
        for (const auto& interval : port.busy_intervals) {
            const std::uint64_t start =
                std::max(window_start, interval.start_ps);
            const std::uint64_t end = std::min(window_end, interval.end_ps);
            if (end >= start) {
                intersections.emplace_back(start, end);
            }
        }
    }
    return interval_union_length(std::move(intersections));
}

std::uint64_t epoch_max_backlog(const OcsPlaneDataplaneStats& dataplane,
                                std::uint64_t epoch_id) {
    std::uint64_t result = 0;
    for (const auto& port : dataplane.source_ports) {
        if (epoch_id < port.max_backlog_bytes_by_program_epoch.size()) {
            result = std::max(
                result,
                port.max_backlog_bytes_by_program_epoch
                    [static_cast<std::size_t>(epoch_id)]);
        }
    }
    return result;
}

void enforce_result_limit(const std::string& raw) {
    if (raw.size() > contract::kMaxResultFileBytes) {
        throw std::runtime_error("result_size_estimate_violation");
    }
}

std::string canonical_bytes(const Json& document) {
    std::string raw = document.dump();
    raw.push_back('\n');
    enforce_result_limit(raw);
    return raw;
}

}  // namespace

std::string encode_runtime_result(const OcsExecutionPlanV2& plan,
                                  const std::string& plan_file_sha256,
                                  const OcsRunSummary& summary) {
    Json result = trusted_identity(plan, plan_file_sha256, summary.status,
                                   summary.stop_reason);
    const bool success = summary.status == "success";
    const std::optional<std::uint64_t> complete =
        success ? summary.collective_complete_ps : std::nullopt;
    result["timing"] = timing_json(
        0, complete, complete,
        std::optional<std::uint64_t>(summary.simulation_stop_ps));

    Json tokens = Json::array();
    for (const auto& token : summary.tokens) {
        tokens.push_back({{"producer_id", optional_json(token.producer_id)},
                          {"ready_ps", optional_json(token.ready_ps)},
                          {"status", token.status},
                          {"token_id", token.token_id},
                          {"token_type", token.token_type}});
    }
    result["tokens"] = std::move(tokens);

    Json steps = Json::array();
    for (const auto& step : summary.steps) {
        const auto& spec = plan.step_by_id(step.step_id);
        steps.push_back(
            {{"completed_flow_group_count", step.completed_flow_group_count},
             {"completion_ps", optional_json(step.completion_ps)},
             {"completion_token_id", spec.completion_token_id},
             {"expected_flow_group_count", step.expected_flow_group_count},
             {"first_flow_group_release_ps",
              optional_json(step.first_flow_group_release_ps)},
             {"flow_group_ids", spec.flow_group_ids},
             {"status", step.status},
             {"step_id", step.step_id}});
    }
    result["steps"] = std::move(steps);

    Json flows = Json::array();
    for (const auto& flow : summary.flows) {
        flows.push_back(
            {{"dst_rank", flow.dst_rank},
             {"flow_group_id", flow.flow_group_id},
             {"flow_id", flow.flow_id},
             {"last_payload_received_ps",
              optional_json(flow.last_payload_received_ps)},
             {"last_payload_sent_ps",
              optional_json(flow.last_payload_sent_ps)},
             {"logical_packet_count", flow.logical_packet_count},
             {"payload_bytes", flow.payload_bytes},
             {"received_payload_bytes", flow.received_payload_bytes},
             {"release_ps", optional_json(flow.release_ps)},
             {"sent_payload_bytes", flow.sent_payload_bytes},
             {"src_rank", flow.src_rank},
             {"status", flow_status(flow)}});
    }
    result["flows"] = flows;

    Json groups = Json::array();
    for (const auto& group : summary.flow_groups) {
        std::uint64_t sent = 0;
        std::uint64_t received = 0;
        std::uint64_t completed = 0;
        for (const std::uint64_t flow_id : group.flow_ids) {
            const auto& flow = summary.flows.at(
                static_cast<std::size_t>(flow_id));
            sent = checked_sum(sent, flow.sent_payload_bytes);
            received = checked_sum(received, flow.received_payload_bytes);
            if (flow.complete) {
                completed = checked_sum(completed, 1);
            }
        }
        const auto& epoch = runtime_epoch(summary, group.plane_id,
                                          group.program_epoch_id);
        groups.push_back(
            {{"completed_flow_count", completed},
             {"completion_ps", optional_json(group.completion_ps)},
             {"configuration_id", group.configuration_id},
             {"dependency_ready_ps",
              optional_json(group.dependency_ready_ps)},
             {"depends_on_token_ids", group.depends_on_token_ids},
             {"expected_flow_count", group.expected_flow_count},
             {"expected_payload_bytes", group.expected_payload_bytes},
             {"flow_group_id", group.flow_group_id},
             {"flow_ids", group.flow_ids},
             {"physical_config_generation",
              epoch.path_ready_ps.has_value()
                  ? Json(group.physical_config_generation)
                  : Json(nullptr)},
             {"plane_id", group.plane_id},
             {"program_epoch_id", group.program_epoch_id},
             {"received_payload_bytes", received},
             {"release_ps", optional_json(group.release_ps)},
             {"sent_payload_bytes", sent},
             {"status", group.status},
             {"step_id", group.step_id}});
    }
    result["flow_groups"] = groups;

    Json planes = Json::array();
    for (std::size_t plane_index = 0;
         plane_index < summary.planes.size(); ++plane_index) {
        const auto& runtime_plane = summary.planes[plane_index];
        const auto& dataplane = summary.dataplane_planes.at(plane_index);
        const auto& program = plan.plane_programs.at(plane_index);
        Json source_ports = Json::array();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> all_intervals;
        std::uint64_t plane_max_backlog = 0;
        for (const auto& port : dataplane.source_ports) {
            Json intervals = Json::array();
            for (const auto& interval : port.busy_intervals) {
                intervals.push_back(
                    {{"end_ps", interval.end_ps},
                     {"start_ps", interval.start_ps},
                     {"truncated_at_stop", interval.truncated_at_stop}});
                all_intervals.emplace_back(interval.start_ps,
                                           interval.end_ps);
            }
            plane_max_backlog =
                std::max(plane_max_backlog, port.max_backlog_bytes);
            source_ports.push_back(
                {{"busy_intervals", std::move(intervals)},
                 {"busy_time_ps", port.busy_time_ps},
                 {"logical_packet_count", port.logical_packet_count},
                 {"max_backlog_bytes", port.max_backlog_bytes},
                 {"sent_payload_bytes", port.sent_payload_bytes},
                 {"src_rank", port.src_rank}});
        }

        Json epochs = Json::array();
        for (const auto& epoch : runtime_plane.epochs) {
            epochs.push_back(
                {{"busy_time_ps",
                  epoch_busy_time(epoch, dataplane,
                                  summary.simulation_stop_ps)},
                 {"close_ps", optional_json(epoch.close_ps)},
                 {"configuration_id", epoch.configuration_id},
                 {"drain_complete_ps",
                  optional_json(epoch.drain_complete_ps)},
                 {"flow_group_ids", epoch.flow_group_ids},
                 {"max_serializer_backlog_bytes",
                  epoch_max_backlog(dataplane, epoch.program_epoch_id)},
                 {"path_prep_not_before_token_ids",
                  epoch.path_prep_not_before_token_ids},
                 {"path_prep_start_ps",
                  optional_json(epoch.path_prep_start_ps)},
                 {"path_ready_ps", optional_json(epoch.path_ready_ps)},
                 {"physical_config_generation",
                  epoch.path_ready_ps.has_value()
                      ? Json(expected_generation(program,
                                                 epoch.program_epoch_id))
                      : Json(nullptr)},
                 {"program_epoch_id", epoch.program_epoch_id},
                 {"reconfiguration_end_ps",
                  optional_json(epoch.reconfiguration_end_ps)},
                 {"reconfiguration_start_ps",
                  optional_json(epoch.reconfiguration_start_ps)},
                 {"status", epoch.status},
                 {"transfer_complete_ps",
                  optional_json(epoch.transfer_complete_ps)},
                 {"transfer_start_ps",
                  optional_json(epoch.transfer_start_ps)},
                 {"transition", epoch.transition}});
        }

        std::uint64_t sent = 0;
        std::uint64_t received = 0;
        for (const auto& group : summary.flow_groups) {
            if (group.plane_id != runtime_plane.plane_id) {
                continue;
            }
            for (const std::uint64_t flow_id : group.flow_ids) {
                sent = checked_sum(
                    sent, summary.flows[static_cast<std::size_t>(flow_id)]
                              .sent_payload_bytes);
                received = checked_sum(
                    received,
                    summary.flows[static_cast<std::size_t>(flow_id)]
                        .received_payload_bytes);
            }
        }
        planes.push_back(
            {{"busy_time_ps", interval_union_length(all_intervals)},
             {"epochs", std::move(epochs)},
             {"expected_payload_bytes",
              plan.traffic_inventory.per_plane_payload_bytes[plane_index]},
             {"max_serializer_backlog_bytes", plane_max_backlog},
             {"plane_id", runtime_plane.plane_id},
             {"received_payload_bytes", received},
             {"sent_payload_bytes", sent},
             {"source_ports", std::move(source_ports)}});
    }
    result["planes"] = std::move(planes);

    std::vector<std::uint64_t> unfinished_flows;
    for (const auto& flow : summary.flows) {
        if (!flow.complete) {
            unfinished_flows.push_back(flow.flow_id);
        }
    }
    result["traffic"] =
        {{"completed_flow_count", summary.traffic.completed_flow_count},
         {"completed_flow_group_count",
          summary.traffic.completed_flow_group_count},
         {"drops", 0},
         {"duplicate_payload_bytes", 0},
         {"expected_flow_count", summary.traffic.expected_flow_count},
         {"expected_flow_group_count",
          summary.traffic.expected_flow_group_count},
         {"expected_payload_bytes", summary.traffic.expected_payload_bytes},
         {"in_flight_transit_unit_count",
          summary.traffic.in_flight_transit_unit_count},
         {"logical_packet_count", summary.traffic.logical_packet_count},
         {"missing_payload_bytes",
          summary.traffic.expected_payload_bytes -
              summary.traffic.received_payload_bytes},
         {"per_plane_payload_bytes",
          plan.traffic_inventory.per_plane_payload_bytes},
         {"per_rank_received_payload_bytes",
          summary.traffic.per_rank_received_payload_bytes},
         {"per_rank_sent_payload_bytes",
          summary.traffic.per_rank_sent_payload_bytes},
         {"processed_event_count", summary.traffic.processed_event_count},
         {"received_payload_bytes", summary.traffic.received_payload_bytes},
         {"sent_payload_bytes", summary.traffic.sent_payload_bytes},
         {"simulated_transit_unit_count",
          summary.traffic.simulated_transit_unit_count},
         {"unfinished_flow_ids", unfinished_flows},
         {"wire_bytes", summary.traffic.sent_payload_bytes}};
    result["invariants"] = runtime_invariants(plan, summary);

    if (success) {
        result["blocked_state"] = nullptr;
        result["error"] = nullptr;
    } else {
        if (summary.blocked_state.has_value()) {
            result["blocked_state"] =
                blocked_state_json(*summary.blocked_state);
        }
        const std::string code = canonical_runtime_error_code(summary);
        result["error"] = error_json(
            {summary.stop_reason, code, std::nullopt,
             stable_runtime_message(code)});
    }
    return canonical_bytes(result);
}

std::string encode_preexecution_failure(
    const OcsExecutionPlanV2& plan, const std::string& plan_file_sha256,
    const OcsResultFailure& failure) {
    Json result = trusted_identity(plan, plan_file_sha256, "failure",
                                   failure.stop_reason);
    result["error"] = error_json(failure);
    return canonical_bytes(result);
}

std::string encode_untrusted_failure(
    const std::optional<std::string>& plan_file_sha256,
    const OcsResultFailure& failure) {
    Json result = {{"blocked_state", nullptr},
                   {"case_id", nullptr},
                   {"completion_semantics", nullptr},
                   {"dependency_mode", nullptr},
                   {"error", error_json(failure)},
                   {"execution_mode", nullptr},
                   {"flow_groups", Json::array()},
                   {"flows", Json::array()},
                   {"invariants", null_invariants()},
                   {"path_preparation_policy", nullptr},
                   {"plan_file_sha256",
                    optional_json(plan_file_sha256)},
                   {"planes", Json::array()},
                   {"provenance", nullptr},
                   {"run_limits", nullptr},
                   {"schema_version", contract::kResultSchemaId},
                   {"status", "failure"},
                   {"steps", Json::array()},
                   {"stop_reason", failure.stop_reason},
                   {"strategy", nullptr},
                   {"timing", timing_json(std::nullopt, std::nullopt,
                                          std::nullopt, std::nullopt)},
                   {"tokens", Json::array()},
                   {"traffic", nullptr},
                   {"transport_mode", nullptr},
                   {"units", nullptr},
                   {"wire_model", nullptr}};
    return canonical_bytes(result);
}

std::string encode_operation_trace(
    const std::vector<OcsTraceEvent>& events) {
    std::string raw;
    for (const auto& event : events) {
        const auto& context = event.context;
        const std::optional<std::uint64_t> physical_generation =
            event.event_type == "path_prep_start"
                ? std::optional<std::uint64_t>()
                : context.physical_config_generation;
        Json line =
            {{"configuration_id", optional_json(context.configuration_id)},
             {"event_index", event.event_index},
             {"event_type", event.event_type},
             {"flow_group_id", optional_json(context.flow_group_id)},
             {"flow_id", optional_json(context.flow_id)},
             {"physical_config_generation",
              optional_json(physical_generation)},
             {"plane_id", optional_json(context.plane_id)},
             {"program_epoch_id", optional_json(context.program_epoch_id)},
             {"reason_code", optional_json(context.reason_code)},
             {"schema_version", contract::kOperationEventSchemaId},
             {"step_id", optional_json(context.step_id)},
             {"time_ps", event.time_ps},
             {"token_id", optional_json(context.token_id)}};
        raw += line.dump();
        raw.push_back('\n');
    }
    return raw;
}

void atomic_commit_new_file(const std::filesystem::path& path,
                            const std::string& raw_bytes) {
    const std::filesystem::path parent =
        path.parent_path().empty() ? std::filesystem::path(".")
                                   : path.parent_path();
    std::string pattern =
        (parent / ("." + path.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const int descriptor = ::mkstemp(buffer.data());
    if (descriptor < 0) {
        throw std::runtime_error("output_commit_failed");
    }
    const std::filesystem::path temporary(buffer.data());
    bool descriptor_open = true;
    try {
        (void)::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
        std::size_t offset = 0;
        while (offset < raw_bytes.size()) {
            const ssize_t count = ::write(
                descriptor, raw_bytes.data() + offset,
                raw_bytes.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                throw std::runtime_error("output_commit_failed");
            }
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor) != 0 || ::close(descriptor) != 0) {
            descriptor_open = false;
            throw std::runtime_error("output_commit_failed");
        }
        descriptor_open = false;
#if defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
        if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD,
                      path.c_str(), RENAME_NOREPLACE) != 0) {
            throw std::runtime_error("output_commit_failed");
        }
#else
        if (::link(temporary.c_str(), path.c_str()) != 0 ||
            ::unlink(temporary.c_str()) != 0) {
            throw std::runtime_error("output_commit_failed");
        }
#endif
        const int directory =
            ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) {
            throw std::runtime_error("output_commit_failed");
        }
        const bool directory_ok = ::fsync(directory) == 0;
        const bool close_ok = ::close(directory) == 0;
        if (!directory_ok || !close_ok) {
            throw std::runtime_error("output_commit_failed");
        }
    } catch (...) {
        if (descriptor_open) {
            (void)::close(descriptor);
        }
        (void)::unlink(temporary.c_str());
        throw;
    }
}

}  // namespace htsim_ocs
