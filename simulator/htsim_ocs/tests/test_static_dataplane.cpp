#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "ocs_dataplane_error.h"
#include "ocs_execution_mode.h"
#include "ocs_plan_parser.h"
#include "ocs_topology.h"
#include "ocs_validation_error.h"

namespace {

using json = nlohmann::json;

json optional_u64(const std::optional<std::uint64_t>& value) {
    return value.has_value() ? json(*value) : json(nullptr);
}

json audit_json(const htsim_ocs::OcsStaticDataplaneAudit& audit) {
    json flows = json::array();
    for (const auto& flow : audit.flows) {
        flows.push_back({{"complete", flow.complete},
                         {"dst_rank", flow.dst_rank},
                         {"flow_group_id", flow.flow_group_id},
                         {"flow_id", flow.flow_id},
                         {"last_payload_received_ps",
                          optional_u64(flow.last_payload_received_ps)},
                         {"last_payload_sent_ps",
                          optional_u64(flow.last_payload_sent_ps)},
                         {"logical_packet_count", flow.logical_packet_count},
                         {"payload_bytes", flow.payload_bytes},
                         {"received_payload_bytes", flow.received_payload_bytes},
                         {"release_ps", optional_u64(flow.release_ps)},
                         {"sent_payload_bytes", flow.sent_payload_bytes},
                         {"src_rank", flow.src_rank},
                         {"tail_payload_bytes", flow.tail_payload_bytes}});
    }
    json planes = json::array();
    for (const auto& plane : audit.planes) {
        json ports = json::array();
        for (const auto& port : plane.source_ports) {
            json intervals = json::array();
            for (const auto& interval : port.busy_intervals) {
                intervals.push_back({{"end_ps", interval.end_ps},
                                     {"start_ps", interval.start_ps}});
            }
            ports.push_back({{"busy_intervals", std::move(intervals)},
                             {"busy_time_ps", port.busy_time_ps},
                             {"logical_packet_count", port.logical_packet_count},
                             {"max_backlog_bytes", port.max_backlog_bytes},
                             {"plane_id", port.plane_id},
                             {"sent_payload_bytes", port.sent_payload_bytes},
                             {"simulated_transit_unit_count",
                              port.simulated_transit_unit_count},
                             {"src_rank", port.src_rank}});
        }
        planes.push_back(
            {{"in_flight_transit_unit_count",
              plane.in_flight_transit_unit_count},
             {"logical_packet_count", plane.logical_packet_count},
             {"plane_id", plane.plane_id},
             {"received_payload_bytes", plane.received_payload_bytes},
             {"sent_payload_bytes", plane.sent_payload_bytes},
             {"simulated_transit_unit_count",
              plane.simulated_transit_unit_count},
             {"source_ports", std::move(ports)}});
    }
    return {{"all_pools_returned", audit.all_pools_returned},
            {"all_routes_exact", audit.all_routes_exact},
            {"batch_pool_allocated_count", audit.batch_pool_allocated_count},
            {"batch_pool_peak_in_use", audit.batch_pool_peak_in_use},
            {"collective_complete_ps", audit.collective_complete_ps},
            {"completed_flow_count", audit.completed_flow_count},
            {"execution_mode", audit.execution_mode},
            {"expected_flow_count", audit.expected_flow_count},
            {"expected_payload_bytes", audit.expected_payload_bytes},
            {"flows", std::move(flows)},
            {"in_flight_transit_unit_count",
             audit.in_flight_transit_unit_count},
            {"logical_packet_count", audit.logical_packet_count},
            {"packet_pool_allocated_count", audit.packet_pool_allocated_count},
            {"packet_pool_peak_in_use", audit.packet_pool_peak_in_use},
            {"per_plane_payload_bytes", audit.per_plane_payload_bytes},
            {"per_rank_received_payload_bytes",
             audit.per_rank_received_payload_bytes},
            {"per_rank_sent_payload_bytes",
             audit.per_rank_sent_payload_bytes},
            {"planes", std::move(planes)},
            {"processed_event_count", audit.processed_event_count},
            {"received_payload_bytes", audit.received_payload_bytes},
            {"sent_payload_bytes", audit.sent_payload_bytes},
            {"simulated_transit_unit_count",
             audit.simulated_transit_unit_count}};
}

}  // namespace

int main(int argc, char** argv) {
    using namespace htsim_ocs;
    if ((argc != 5 && argc != 6) || std::string(argv[1]) != "--plan" ||
        std::string(argv[3]) != "--execution-mode") {
        std::cerr << "usage: test_static_dataplane --plan PATH "
                     "--execution-mode MODE [--preflight-only]\n";
        return 2;
    }
    try {
        ParsedExecutionPlan parsed = OcsPlanParser::parse_file(argv[2]);
        if (argc == 6) {
            if (std::string(argv[5]) != "--preflight-only") {
                throw std::invalid_argument("unknown static driver option");
            }
            const OcsExecutionMode mode = parse_execution_mode(argv[4]);
            const OcsCapacityProof proof =
                prove_capacity_before_event_sources(*parsed.plan, mode);
            std::cout
                << json({{"conservative_max_live_object_upper_bound",
                          proof.conservative_max_live_object_upper_bound},
                         {"execution_mode", execution_mode_name(mode)},
                         {"per_pipe_transit_unit_upper_bound",
                          proof.per_pipe_transit_unit_upper_bound},
                         {"preflight", true}})
                       .dump()
                << '\n';
            return 0;
        }
        std::vector<std::uint64_t> group_ids;
        group_ids.reserve(parsed.plan->flow_groups.size());
        for (const auto& group : parsed.plan->flow_groups) {
            group_ids.push_back(group.flow_group_id);
        }
        OcsStaticDataplaneAudit audit =
            run_static_dataplane(parsed.plan, group_ids, argv[4]);
        std::cout << audit_json(audit).dump() << '\n';
        return 0;
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
