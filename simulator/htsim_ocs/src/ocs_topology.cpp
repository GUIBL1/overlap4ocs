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
                         OcsCapacityProof capacity_proof,
                         FlowCallback flow_completion_callback,
                         FlowCallback flow_last_sent_callback)
    : event_list_(event_list),
      plan_(std::move(plan)),
      mode_(mode),
      capacity_proof_(std::move(capacity_proof)),
      flows_(plan_->flows.size()),
      selected_flows_(plan_->flows.size(), false),
      completed_flows_(plan_->flows.size(), false),
      flow_completion_callback_(std::move(flow_completion_callback)),
      flow_last_sent_callback_(std::move(flow_last_sent_callback)),
      processed_event_count_at_start_(EventList::processedEventCount()) {
    if (capacity_proof_.execution_mode != mode_ ||
        capacity_proof_.per_pipe_transit_unit_upper_bound.size() !=
            plan_->topology.plane_count) {
        throw OcsDataplaneError("capacity_proof_mismatch",
                                "capacity proof does not match topology");
    }
    materialize_all_flows();
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

std::uint64_t OcsTopology::physical_generation_for_epoch(
    std::uint64_t plane_id, std::uint64_t program_epoch_id) const {
    const OcsPlaneProgram& program =
        plan_->plane_program_by_id(plane_id);
    if (program_epoch_id >= program.epochs.size()) {
        throw OcsDataplaneError("unknown_program_epoch",
                                "program epoch is outside the plane program");
    }
    std::uint64_t physical_config_generation = 0;
    for (const OcsProgramEpochSpec& epoch : program.epochs) {
        if (epoch.program_epoch_id > program_epoch_id) {
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
    return physical_config_generation;
}

void OcsTopology::materialize_all_flows() {
    for (const OcsFlowSpec& spec : plan_->flows) {
        const OcsFlowGroupSpec& group =
            plan_->flow_group_by_id(spec.flow_group_id);
        const std::uint64_t generation = physical_generation_for_epoch(
            group.plane_id, group.program_epoch_id);
        flows_[static_cast<std::size_t>(spec.flow_id)] =
            std::make_unique<OcsFlow>(
                spec,
                OcsFlowRuntimeIdentity{group.step_id, group.plane_id,
                                       group.program_epoch_id,
                                       group.configuration_id, generation},
                [this](const OcsFlowStats& stats) {
                    note_flow_complete(stats);
                },
                [this](const OcsFlowStats& stats) {
                    note_flow_last_sent(stats);
                });
    }
}

void OcsTopology::install_epoch_path(std::uint64_t plane_id,
                                     std::uint64_t program_epoch_id) {
    const OcsPlaneProgram& program = plan_->plane_program_by_id(plane_id);
    if (program_epoch_id >= program.epochs.size()) {
        throw OcsDataplaneError("unknown_program_epoch",
                                "program epoch is outside the plane program");
    }
    const OcsProgramEpochSpec& epoch =
        program.epochs[static_cast<std::size_t>(program_epoch_id)];
    const OcsConfiguration& configuration =
        plan_->configuration_by_id(epoch.configuration_id);
    plane_by_id(plane_id).install_path(
        program_epoch_id, epoch.configuration_id,
        physical_generation_for_epoch(plane_id, program_epoch_id),
        configuration.permutation);
}

void OcsTopology::release_group(std::uint64_t flow_group_id) {
    const OcsFlowGroupSpec& group = plan_->flow_group_by_id(flow_group_id);
    OcsPlaneDataplane& plane = plane_by_id(group.plane_id);
    const std::uint64_t generation = physical_generation_for_epoch(
        group.plane_id, group.program_epoch_id);
    if (!plane.path_installed() ||
        plane.active_program_epoch_id() != group.program_epoch_id ||
        plane.active_configuration_id() != group.configuration_id ||
        plane.active_physical_config_generation() != generation) {
        throw OcsDataplaneError("epoch_violation",
                                "flow group path is not active");
    }

    std::vector<OcsFlow*> group_flows;
    group_flows.reserve(group.flow_ids.size());
    for (std::uint64_t flow_id : group.flow_ids) {
        if (selected_flows_[static_cast<std::size_t>(flow_id)]) {
            throw OcsDataplaneError("duplicate_flow_release",
                                    "flow was selected by two static groups");
        }
        const OcsFlowSpec& spec = plan_->flow_by_id(flow_id);
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

void OcsTopology::materialize_group(const OcsFlowGroupSpec& group) {
    OcsPlaneDataplane& plane = plane_by_id(group.plane_id);
    const OcsConfiguration& configuration =
        plan_->configuration_by_id(group.configuration_id);
    const std::uint64_t generation = physical_generation_for_epoch(
        group.plane_id, group.program_epoch_id);
    plane.activate_static_path(group.program_epoch_id, group.configuration_id,
                               generation, configuration.permutation);
    release_group(group.flow_group_id);
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
    if (flow_completion_callback_) {
        flow_completion_callback_(stats);
    }
}

void OcsTopology::note_flow_last_sent(const OcsFlowStats& stats) {
    if (stats.flow_id >= selected_flows_.size() ||
        !selected_flows_[static_cast<std::size_t>(stats.flow_id)]) {
        throw OcsDataplaneError("flow_send_before_release",
                                "last-sent callback refers to an unreleased flow");
    }
    if (flow_last_sent_callback_) {
        flow_last_sent_callback_(stats);
    }
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

bool OcsTopology::plane_data_plane_idle(std::uint64_t plane_id) const {
    if (plane_id >= planes_.size()) {
        throw OcsDataplaneError("unknown_plane", "plane ID is outside topology");
    }
    return planes_[static_cast<std::size_t>(plane_id)]->data_plane_idle();
}

void OcsTopology::abort_pending() {
    for (auto& plane : planes_) {
        plane->abort_pending();
    }
    packet_pool_.verify_all_returned();
    batch_pool_.verify_all_returned();
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

const OcsPlaneDataplane& OcsTopology::plane_by_id(
    std::uint64_t plane_id) const {
    if (plane_id >= planes_.size()) {
        throw OcsDataplaneError("unknown_plane", "plane ID is outside topology");
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
        if (flows_[flow_id] == nullptr || !selected_flows_[flow_id]) {
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

std::vector<OcsFlowStats> OcsTopology::all_flow_stats() const {
    std::vector<OcsFlowStats> result;
    result.reserve(flows_.size());
    for (const auto& flow : flows_) {
        if (flow == nullptr) {
            throw OcsDataplaneError("unknown_runtime_flow",
                                    "runtime flow was not materialized");
        }
        result.push_back(flow->stats());
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
