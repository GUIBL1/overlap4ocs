#include <cstdint>
#include <iostream>
#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "ocs_coordinator.h"
#include "ocs_dataplane_error.h"
#include "ocs_plan_parser.h"
#include "ocs_validation_error.h"

namespace {

using json = nlohmann::json;

template <typename T>
json optional_json(const std::optional<T>& value) {
    return value.has_value() ? json(*value) : json(nullptr);
}

json trace_json(const htsim_ocs::OcsTraceEvent& event) {
    const auto& context = event.context;
    return {{"configuration_id", optional_json(context.configuration_id)},
            {"event_index", event.event_index},
            {"event_type", event.event_type},
            {"flow_group_id", optional_json(context.flow_group_id)},
            {"flow_id", optional_json(context.flow_id)},
            {"physical_config_generation",
             optional_json(context.physical_config_generation)},
            {"plane_id", optional_json(context.plane_id)},
            {"program_epoch_id", optional_json(context.program_epoch_id)},
            {"reason_code", optional_json(context.reason_code)},
            {"step_id", optional_json(context.step_id)},
            {"time_ps", event.time_ps},
            {"token_id", optional_json(context.token_id)}};
}

json epoch_json(const htsim_ocs::OcsEpochRuntimeStats& epoch) {
    return {{"close_ps", optional_json(epoch.close_ps)},
            {"configuration_id", epoch.configuration_id},
            {"drain_complete_ps", optional_json(epoch.drain_complete_ps)},
            {"flow_group_ids", epoch.flow_group_ids},
            {"path_prep_not_before_token_ids",
             epoch.path_prep_not_before_token_ids},
            {"path_prep_start_ps", optional_json(epoch.path_prep_start_ps)},
            {"path_ready_ps", optional_json(epoch.path_ready_ps)},
            {"physical_config_generation",
             epoch.physical_config_generation},
            {"program_epoch_id", epoch.program_epoch_id},
            {"reconfiguration_end_ps",
             optional_json(epoch.reconfiguration_end_ps)},
            {"reconfiguration_start_ps",
             optional_json(epoch.reconfiguration_start_ps)},
            {"status", epoch.status},
            {"transfer_complete_ps",
             optional_json(epoch.transfer_complete_ps)},
            {"transfer_start_ps", optional_json(epoch.transfer_start_ps)},
            {"transition", epoch.transition}};
}

json plane_json(const htsim_ocs::OcsPlaneRuntimeStats& plane) {
    json epochs = json::array();
    for (const auto& epoch : plane.epochs) {
        epochs.push_back(epoch_json(epoch));
    }
    return {{"active_configuration_id",
             optional_json(plane.active_configuration_id)},
            {"active_program_epoch_id",
             optional_json(plane.active_program_epoch_id)},
            {"epochs", std::move(epochs)},
            {"in_flight_transit_unit_count",
             plane.in_flight_transit_unit_count},
            {"pending_path_prep_token_ids",
             plane.pending_path_prep_token_ids},
            {"physical_config_generation",
             optional_json(plane.physical_config_generation)},
            {"plane_id", plane.plane_id},
            {"program_cursor", plane.program_cursor},
            {"serializer_backlog_bytes", plane.serializer_backlog_bytes},
            {"serializer_backlog_flow_count",
             plane.serializer_backlog_flow_count},
            {"state", plane.state}};
}

json summary_json(const htsim_ocs::OcsRunSummary& summary) {
    json tokens = json::array();
    for (const auto& token : summary.tokens) {
        tokens.push_back({{"producer_id", optional_json(token.producer_id)},
                          {"ready_ps", optional_json(token.ready_ps)},
                          {"status", token.status},
                          {"token_id", token.token_id},
                          {"token_type", token.token_type}});
    }
    json steps = json::array();
    for (const auto& step : summary.steps) {
        steps.push_back(
            {{"completed_flow_group_count", step.completed_flow_group_count},
             {"completion_ps", optional_json(step.completion_ps)},
             {"expected_flow_group_count", step.expected_flow_group_count},
             {"first_flow_group_release_ps",
              optional_json(step.first_flow_group_release_ps)},
             {"status", step.status},
             {"step_id", step.step_id}});
    }
    json groups = json::array();
    for (const auto& group : summary.flow_groups) {
        groups.push_back(
            {{"completed_flow_count", group.completed_flow_count},
             {"completion_ps", optional_json(group.completion_ps)},
             {"configuration_id", group.configuration_id},
             {"dependency_ready_ps", optional_json(group.dependency_ready_ps)},
             {"depends_on_token_ids", group.depends_on_token_ids},
             {"expected_flow_count", group.expected_flow_count},
             {"expected_payload_bytes", group.expected_payload_bytes},
             {"flow_group_id", group.flow_group_id},
             {"flow_ids", group.flow_ids},
             {"physical_config_generation",
              group.physical_config_generation},
             {"plane_id", group.plane_id},
             {"program_epoch_id", group.program_epoch_id},
             {"release_ps", optional_json(group.release_ps)},
             {"status", group.status},
             {"step_id", group.step_id}});
    }
    json flows = json::array();
    for (const auto& flow : summary.flows) {
        flows.push_back(
            {{"complete", flow.complete},
             {"dst_rank", flow.dst_rank},
             {"flow_group_id", flow.flow_group_id},
             {"flow_id", flow.flow_id},
             {"last_payload_received_ps",
              optional_json(flow.last_payload_received_ps)},
             {"last_payload_sent_ps", optional_json(flow.last_payload_sent_ps)},
             {"logical_packet_count", flow.logical_packet_count},
             {"payload_bytes", flow.payload_bytes},
             {"received_payload_bytes", flow.received_payload_bytes},
             {"release_ps", optional_json(flow.release_ps)},
             {"sent_payload_bytes", flow.sent_payload_bytes},
             {"src_rank", flow.src_rank},
             {"tail_payload_bytes", flow.tail_payload_bytes}});
    }
    json planes = json::array();
    for (const auto& plane : summary.planes) {
        planes.push_back(plane_json(plane));
    }
    json trace = json::array();
    for (const auto& event : summary.trace) {
        trace.push_back(trace_json(event));
    }
    json blocked = nullptr;
    if (summary.blocked_state.has_value()) {
        json blocked_planes = json::array();
        for (const auto& plane : summary.blocked_state->planes) {
            blocked_planes.push_back(plane_json(plane));
        }
        blocked = {{"next_event_time_ps",
                    optional_json(summary.blocked_state->next_event_time_ps)},
                   {"planes", std::move(blocked_planes)},
                   {"unfinished_flow_group_ids",
                    summary.blocked_state->unfinished_flow_group_ids},
                   {"unfinished_flow_ids",
                    summary.blocked_state->unfinished_flow_ids},
                   {"unfinished_step_ids",
                    summary.blocked_state->unfinished_step_ids},
                   {"unfinished_token_ids",
                    summary.blocked_state->unfinished_token_ids}};
    }
    const auto& traffic = summary.traffic;
    return {
        {"blocked_state", std::move(blocked)},
        {"collective_complete_ps",
         optional_json(summary.collective_complete_ps)},
        {"dependency_mode", summary.dependency_mode},
        {"error_code", optional_json(summary.error_code)},
        {"execution_mode", summary.execution_mode},
        {"flow_groups", std::move(groups)},
        {"flows", std::move(flows)},
        {"path_preparation_policy", summary.path_preparation_policy},
        {"planes", std::move(planes)},
        {"simulation_stop_ps", summary.simulation_stop_ps},
        {"status", summary.status},
        {"steps", std::move(steps)},
        {"stop_reason", summary.stop_reason},
        {"tokens", std::move(tokens)},
        {"trace", std::move(trace)},
        {"traffic",
         {{"completed_flow_count", traffic.completed_flow_count},
          {"completed_flow_group_count",
           traffic.completed_flow_group_count},
          {"expected_flow_count", traffic.expected_flow_count},
          {"expected_flow_group_count",
           traffic.expected_flow_group_count},
          {"expected_payload_bytes", traffic.expected_payload_bytes},
          {"in_flight_transit_unit_count",
           traffic.in_flight_transit_unit_count},
          {"logical_packet_count", traffic.logical_packet_count},
          {"per_plane_payload_bytes", traffic.per_plane_payload_bytes},
          {"per_rank_received_payload_bytes",
           traffic.per_rank_received_payload_bytes},
          {"per_rank_sent_payload_bytes",
           traffic.per_rank_sent_payload_bytes},
          {"processed_event_count", traffic.processed_event_count},
          {"received_payload_bytes", traffic.received_payload_bytes},
          {"sent_payload_bytes", traffic.sent_payload_bytes},
          {"simulated_transit_unit_count",
           traffic.simulated_transit_unit_count}}}};
}

}  // namespace

int main(int argc, char** argv) {
    using namespace htsim_ocs;
    if ((argc != 3 && argc != 5) || std::string(argv[1]) != "--plan" ||
        (argc == 5 && std::string(argv[3]) != "--fault")) {
        std::cerr << "usage: test_runtime_driver --plan PATH "
                     "[--fault suppress-flow-completion=ID|"
                     "duplicate-flow-completion=ID]\n";
        return 2;
    }
    try {
        ParsedExecutionPlan parsed = OcsPlanParser::parse_file(argv[2]);
        OcsRuntimeFaultInjection fault;
        if (argc == 5) {
            const std::string value = argv[4];
            const std::string suppress = "suppress-flow-completion=";
            const std::string duplicate = "duplicate-flow-completion=";
            if (value.rfind(suppress, 0) == 0) {
                fault.suppress_flow_completion_id =
                    std::stoull(value.substr(suppress.size()));
            } else if (value.rfind(duplicate, 0) == 0) {
                fault.duplicate_flow_completion_id =
                    std::stoull(value.substr(duplicate.size()));
            } else {
                throw std::invalid_argument("unknown fault injection");
            }
        }
        OcsRunSummary summary = run_ocs_runtime(parsed.plan, fault);
        std::cout << summary_json(summary).dump() << '\n';
        if (summary.status == "success") {
            return 0;
        }
        if (summary.stop_reason == "internal_error") {
            return 5;
        }
        return 3;
    } catch (const OcsDataplaneError& error) {
        std::cerr << "error_code=" << error.error_code()
                  << " message=" << error.what() << '\n';
        return static_cast<int>(error.exit_code());
    } catch (const OcsValidationError& error) {
        std::cerr << "error_code=" << error.error_code()
                  << " json_pointer=" << error.json_pointer()
                  << " message=" << error.what() << '\n';
        return static_cast<int>(error.exit_code());
    } catch (const std::exception& error) {
        std::cerr << "error_code=internal_error message=" << error.what()
                  << '\n';
        return 5;
    }
}
