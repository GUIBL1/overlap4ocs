#include "ocs_topology.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "checked_arithmetic.h"
#include "eventlist.h"
#include "ocs_dataplane_error.h"

namespace htsim_ocs {
namespace {

void checked_audit_add(std::uint64_t& destination, std::uint64_t increment,
                       const char* error_code) {
    if (!checked_add_u64(destination, increment, destination)) {
        throw OcsDataplaneError(error_code, "data-plane audit counter overflow");
    }
}

}  // namespace

OcsTopology::OcsTopology(EventList& event_list,
                         std::shared_ptr<const OcsExecutionPlanV2> plan,
                         OcsExecutionMode mode,
                         OcsCapacityProof capacity_proof)
    : event_list_(event_list),
      plan_(std::move(plan)),
      mode_(mode),
      capacity_proof_(std::move(capacity_proof)),
      flows_(plan_->flows.size()),
      selected_flows_(plan_->flows.size(), false),
      completed_flows_(plan_->flows.size(), false),
      processed_event_count_at_start_(EventList::processedEventCount()) {
    if (capacity_proof_.execution_mode != mode_ ||
        capacity_proof_.per_pipe_transit_unit_upper_bound.size() !=
            plan_->topology.plane_count) {
        throw OcsDataplaneError("capacity_proof_mismatch",
                                "capacity proof does not match topology");
    }
    planes_.reserve(static_cast<std::size_t>(plan_->topology.plane_count));
    for (std::uint64_t plane_id = 0;
         plane_id < plan_->topology.plane_count; ++plane_id) {
        planes_.push_back(std::make_unique<OcsPlaneDataplane>(
            event_list_, plane_id, plan_->topology.node_count,
            plan_->topology.per_plane_bps, plan_->topology.data_latency_ps,
            plan_->transport.mtu_bytes, mode_, packet_pool_, batch_pool_,
            [this](std::uint64_t flow_id) -> OcsFlow& {
                return flow_by_id(flow_id);
            }));
    }
}

OcsTopology::~OcsTopology() = default;

void OcsTopology::materialize_group(const OcsFlowGroupSpec& group) {
    OcsPlaneDataplane& plane = plane_by_id(group.plane_id);
    const OcsConfiguration& configuration =
        plan_->configuration_by_id(group.configuration_id);
    const OcsPlaneProgram& program =
        plan_->plane_program_by_id(group.plane_id);
    std::uint64_t physical_config_generation = 0;
    for (const OcsProgramEpochSpec& epoch : program.epochs) {
        if (epoch.program_epoch_id > group.program_epoch_id) {
            break;
        }
        if (epoch.transition == "reconfigure") {
            if (!checked_add_u64(physical_config_generation, 1,
                                 physical_config_generation)) {
                throw OcsDataplaneError(
                    "physical_generation_overflow",
                    "physical configuration generation overflow");
            }
        }
    }
    plane.activate_static_path(group.program_epoch_id, group.configuration_id,
                               physical_config_generation,
                               configuration.permutation);

    std::vector<OcsFlow*> group_flows;
    group_flows.reserve(group.flow_ids.size());
    for (std::uint64_t flow_id : group.flow_ids) {
        if (selected_flows_[static_cast<std::size_t>(flow_id)]) {
            throw OcsDataplaneError("duplicate_flow_release",
                                    "flow was selected by two static groups");
        }
        const OcsFlowSpec& spec = plan_->flow_by_id(flow_id);
        flows_[static_cast<std::size_t>(flow_id)] = std::make_unique<OcsFlow>(
            spec,
            OcsFlowRuntimeIdentity{group.step_id,
                                   group.plane_id,
                                   group.program_epoch_id,
                                   group.configuration_id,
                                   physical_config_generation},
            [this](const OcsFlowStats& stats) { note_flow_complete(stats); });
        selected_flows_[static_cast<std::size_t>(flow_id)] = true;
        checked_audit_add(selected_flow_count_, 1, "selected_flow_overflow");
        if (!checked_add_u64(selected_payload_bytes_, spec.payload_bytes,
                             selected_payload_bytes_)) {
            throw OcsDataplaneError("selected_payload_overflow",
                                    "selected payload total overflow");
        }
        group_flows.push_back(flows_[static_cast<std::size_t>(flow_id)].get());
    }
    plane.enqueue_flows(std::move(group_flows));
}

void OcsTopology::release_static_groups(
    std::vector<std::uint64_t> flow_group_ids) {
    std::sort(flow_group_ids.begin(), flow_group_ids.end());
    if (std::adjacent_find(flow_group_ids.begin(), flow_group_ids.end()) !=
        flow_group_ids.end()) {
        throw OcsDataplaneError("duplicate_flow_group_release",
                                "static group list contains a duplicate");
    }
    for (std::uint64_t group_id : flow_group_ids) {
        materialize_group(plan_->flow_group_by_id(group_id));
    }
}

void OcsTopology::note_flow_complete(const OcsFlowStats& stats) {
    if (stats.flow_id >= completed_flows_.size() ||
        !selected_flows_[static_cast<std::size_t>(stats.flow_id)] ||
        completed_flows_[static_cast<std::size_t>(stats.flow_id)]) {
        throw OcsDataplaneError("duplicate_flow_completion",
                                "flow completion callback is invalid");
    }
    completed_flows_[static_cast<std::size_t>(stats.flow_id)] = true;
    checked_audit_add(completed_flow_count_, 1, "completed_flow_overflow");
}

void OcsTopology::run_until_idle() {
    while (EventList::doNextEvent()) {
    }
    if (!data_plane_idle()) {
        throw OcsDataplaneError("deadlock",
                                "event list drained before the data plane");
    }
    packet_pool_.verify_all_returned();
    batch_pool_.verify_all_returned();
}

bool OcsTopology::data_plane_idle() const noexcept {
    return std::all_of(planes_.begin(), planes_.end(), [](const auto& plane) {
        return plane->data_plane_idle();
    });
}

OcsFlow& OcsTopology::flow_by_id(std::uint64_t flow_id) {
    if (flow_id >= flows_.size() ||
        flows_[static_cast<std::size_t>(flow_id)] == nullptr) {
        throw OcsDataplaneError("unknown_runtime_flow",
                                "packet refers to an inactive runtime flow");
    }
    return *flows_[static_cast<std::size_t>(flow_id)];
}

OcsPlaneDataplane& OcsTopology::plane_by_id(std::uint64_t plane_id) {
    if (plane_id >= planes_.size()) {
        throw OcsDataplaneError("unknown_plane",
                                "plane ID is outside topology");
    }
    return *planes_[static_cast<std::size_t>(plane_id)];
}

OcsStaticDataplaneAudit OcsTopology::audit() const {
    OcsStaticDataplaneAudit result;
    result.execution_mode = execution_mode_name(mode_);
    result.expected_flow_count = selected_flow_count_;
    result.completed_flow_count = completed_flow_count_;
    result.expected_payload_bytes = selected_payload_bytes_;
    result.sent_payload_bytes = 0;
    result.received_payload_bytes = 0;
    result.logical_packet_count = 0;
    result.simulated_transit_unit_count = 0;
    result.processed_event_count =
        EventList::processedEventCount() - processed_event_count_at_start_;
    result.in_flight_transit_unit_count = 0;
    result.collective_complete_ps = 0;
    result.packet_pool_allocated_count = packet_pool_.allocated_count();
    result.packet_pool_peak_in_use = packet_pool_.peak_in_use_count();
    result.batch_pool_allocated_count = batch_pool_.allocated_count();
    result.batch_pool_peak_in_use = batch_pool_.peak_in_use_count();
    result.all_routes_exact = true;
    result.all_pools_returned = packet_pool_.in_use_count() == 0 &&
                                batch_pool_.in_use_count() == 0;
    result.per_rank_sent_payload_bytes.assign(
        static_cast<std::size_t>(plan_->topology.node_count), 0);
    result.per_rank_received_payload_bytes.assign(
        static_cast<std::size_t>(plan_->topology.node_count), 0);
    result.per_plane_payload_bytes.assign(
        static_cast<std::size_t>(plan_->topology.plane_count), 0);

    for (std::size_t flow_id = 0; flow_id < flows_.size(); ++flow_id) {
        if (flows_[flow_id] == nullptr) {
            continue;
        }
        OcsFlowStats flow = flows_[flow_id]->stats();
        checked_audit_add(result.sent_payload_bytes, flow.sent_payload_bytes,
                          "audit_sent_bytes_overflow");
        checked_audit_add(result.received_payload_bytes,
                          flow.received_payload_bytes,
                          "audit_received_bytes_overflow");
        checked_audit_add(result.logical_packet_count,
                          flow.logical_packet_count,
                          "audit_packet_count_overflow");
        checked_audit_add(
            result.per_rank_sent_payload_bytes
                [static_cast<std::size_t>(flow.src_rank)],
            flow.sent_payload_bytes, "audit_rank_sent_bytes_overflow");
        checked_audit_add(
            result.per_rank_received_payload_bytes
                [static_cast<std::size_t>(flow.dst_rank)],
            flow.received_payload_bytes,
            "audit_rank_received_bytes_overflow");
        const std::uint64_t plane_id = flows_[flow_id]->identity().plane_id;
        checked_audit_add(
            result.per_plane_payload_bytes[static_cast<std::size_t>(plane_id)],
            flow.sent_payload_bytes, "audit_plane_bytes_overflow");
        if (flow.last_payload_received_ps.has_value()) {
            result.collective_complete_ps = std::max(
                result.collective_complete_ps,
                *flow.last_payload_received_ps);
        }
        result.flows.push_back(std::move(flow));
    }
    result.planes.reserve(planes_.size());
    for (const auto& plane : planes_) {
        OcsPlaneDataplaneStats plane_stats = plane->stats();
        checked_audit_add(result.simulated_transit_unit_count,
                          plane_stats.simulated_transit_unit_count,
                          "audit_transit_unit_count_overflow");
        checked_audit_add(result.in_flight_transit_unit_count,
                          plane_stats.in_flight_transit_unit_count,
                          "audit_in_flight_count_overflow");
        result.all_routes_exact =
            result.all_routes_exact && plane->routes_are_exact();
        result.planes.push_back(std::move(plane_stats));
    }
    return result;
}

OcsStaticDataplaneAudit run_static_dataplane(
    std::shared_ptr<const OcsExecutionPlanV2> plan,
    const std::vector<std::uint64_t>& flow_group_ids,
    const std::string& requested_execution_mode) {
    const OcsExecutionMode mode = parse_execution_mode(requested_execution_mode);
    OcsCapacityProof proof = prove_capacity_before_event_sources(*plan, mode);
    EventList event_list;
    OcsTopology topology(event_list, std::move(plan), mode, std::move(proof));
    topology.release_static_groups(flow_group_ids);
    topology.run_until_idle();
    OcsStaticDataplaneAudit result = topology.audit();
    if (result.completed_flow_count != result.expected_flow_count ||
        result.sent_payload_bytes != result.expected_payload_bytes ||
        result.received_payload_bytes != result.expected_payload_bytes ||
        result.in_flight_transit_unit_count != 0 ||
        !result.all_routes_exact || !result.all_pools_returned) {
        throw OcsDataplaneError("dataplane_audit_mismatch",
                                "static data-plane audit did not balance");
    }
    return result;
}

}  // namespace htsim_ocs
