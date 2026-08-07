#include "ocs_plane_dataplane.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include "checked_arithmetic.h"
#include "ocs_dataplane_error.h"
#include "ocs_flow.h"
#include "ocs_route_table.h"
#include "ocs_sink.h"
#include "ocs_switch.h"
#include "pipe.h"

namespace htsim_ocs {
namespace {

class OcsOwnedPipe final : public Pipe {
  public:
    using Pipe::Pipe;

    void abort_inflight(const std::function<void()>& transit_leave) noexcept {
        EventList::cancelPendingSource(*this);
        while (_count > 0) {
            Packet* packet = _inflight_v[static_cast<std::size_t>(_next_pop)].pkt;
            _next_pop = (_next_pop + 1) % _size;
            --_count;
            if (packet != nullptr) {
                auto* unit = dynamic_cast<OcsTransitUnitView*>(packet);
                if (unit != nullptr && unit->pool_active()) {
                    packet->free();
                }
            }
            if (transit_leave) {
                transit_leave();
            }
        }
        _next_insert = _next_pop;
    }
};

}  // namespace

OcsPlaneDataplane::OcsPlaneDataplane(
    EventList& event_list, std::uint64_t plane_id, std::uint64_t node_count,
    std::uint64_t per_plane_bps, std::uint64_t data_latency_ps,
    std::uint64_t mtu_bytes, OcsExecutionMode mode,
    OcsPacketPool<OcsPacket>& packet_pool,
    OcsPacketPool<OcsTransitBatch>& batch_pool, FlowLookup flow_lookup)
    : plane_id_(plane_id),
      node_count_(node_count),
      mtu_bytes_(mtu_bytes),
      mode_(mode),
      flow_lookup_(std::move(flow_lookup)) {
    switch_ = std::make_unique<OcsSwitch>(
        plane_id_, data_latency_ps, [this]() { transit_enter(); },
        [this]() { transit_leave(); });

    pipes_.reserve(static_cast<std::size_t>(node_count_));
    sinks_.reserve(static_cast<std::size_t>(node_count_));
    std::vector<Pipe*> pipe_ptrs;
    std::vector<OcsSink*> sink_ptrs;
    pipe_ptrs.reserve(static_cast<std::size_t>(node_count_));
    sink_ptrs.reserve(static_cast<std::size_t>(node_count_));
    for (std::uint64_t dst = 0; dst < node_count_; ++dst) {
        auto pipe = std::make_unique<OcsOwnedPipe>(data_latency_ps, event_list);
        std::ostringstream pipe_name;
        pipe_name << "ocs-pipe-plane-" << plane_id_ << "-dst-" << dst;
        pipe->forceName(pipe_name.str());
        pipe_ptrs.push_back(pipe.get());
        pipes_.push_back(std::move(pipe));

        auto sink = std::make_unique<OcsSink>(
            plane_id_, dst,
            [this](const OcsTransitUnitView& unit, std::uint64_t receive_ps) {
                receive_transit_unit(unit, receive_ps);
            },
            [this]() { transit_leave(); });
        sink_ptrs.push_back(sink.get());
        sinks_.push_back(std::move(sink));
    }
    route_table_ =
        std::make_unique<OcsRouteTable>(*switch_, pipe_ptrs, sink_ptrs);

    serializers_.reserve(static_cast<std::size_t>(node_count_));
    for (std::uint64_t src = 0; src < node_count_; ++src) {
        serializers_.push_back(std::make_unique<OcsPortSerializer>(
            event_list, plane_id_, src, per_plane_bps, mtu_bytes, mode_,
            packet_pool, batch_pool, *route_table_));
    }
}

OcsPlaneDataplane::~OcsPlaneDataplane() = default;

void OcsPlaneDataplane::activate_static_path(
    std::uint64_t program_epoch_id, std::uint64_t configuration_id,
    std::uint64_t physical_config_generation,
    const std::vector<std::uint64_t>& permutation) {
    if (switch_->installed() &&
        (switch_->program_epoch_id() != program_epoch_id ||
         switch_->configuration_id() != configuration_id ||
         switch_->physical_config_generation() !=
             physical_config_generation)) {
        throw OcsDataplaneError("static_path_conflict",
                                "static adapter cannot activate two plane states");
    }
    install_path(program_epoch_id, configuration_id,
                 physical_config_generation, permutation);
}

void OcsPlaneDataplane::install_path(
    std::uint64_t program_epoch_id, std::uint64_t configuration_id,
    std::uint64_t physical_config_generation,
    const std::vector<std::uint64_t>& permutation) {
    if (permutation.size() != node_count_) {
        throw OcsDataplaneError("configuration_size_mismatch",
                                "active permutation size does not match plane");
    }
    if (switch_->installed() &&
        switch_->program_epoch_id() == program_epoch_id &&
        switch_->configuration_id() == configuration_id &&
        switch_->physical_config_generation() ==
            physical_config_generation) {
        return;
    }
    if (!data_plane_idle()) {
        throw OcsDataplaneError("epoch_violation",
                                "cannot install a path while the plane is busy");
    }
    switch_->install(program_epoch_id, configuration_id,
                     physical_config_generation, permutation);
}

void OcsPlaneDataplane::enqueue_flows(std::vector<OcsFlow*> flows) {
    std::vector<std::vector<OcsFlow*>> by_source(
        static_cast<std::size_t>(node_count_));
    for (OcsFlow* flow : flows) {
        if (flow == nullptr || flow->identity().plane_id != plane_id_ ||
            flow->spec().src_rank >= node_count_) {
            throw OcsDataplaneError("serializer_port_mismatch",
                                    "flow does not belong to this plane");
        }
        by_source[static_cast<std::size_t>(flow->spec().src_rank)].push_back(
            flow);
    }
    for (std::size_t src = 0; src < by_source.size(); ++src) {
        if (!by_source[src].empty()) {
            serializers_[src]->enqueue_flows(std::move(by_source[src]));
        }
    }
}

void OcsPlaneDataplane::transit_enter() {
    if (!checked_add_u64(in_flight_transit_unit_count_, 1,
                         in_flight_transit_unit_count_)) {
        throw OcsDataplaneError("in_flight_counter_overflow",
                                "plane in-flight counter overflow");
    }
}

void OcsPlaneDataplane::transit_leave() {
    if (in_flight_transit_unit_count_ == 0) {
        throw OcsDataplaneError("in_flight_counter_underflow",
                                "plane in-flight counter underflow");
    }
    --in_flight_transit_unit_count_;
}

void OcsPlaneDataplane::receive_transit_unit(
    const OcsTransitUnitView& unit, std::uint64_t receive_ps) {
    const OcsPacketMetadata& metadata = unit.metadata();
    OcsFlow& flow = flow_lookup_(metadata.flow_id);
    if (flow.spec().flow_group_id != metadata.flow_group_id ||
        flow.spec().src_rank != metadata.src_rank ||
        flow.spec().dst_rank != metadata.dst_rank ||
        flow.identity().step_id != metadata.step_id ||
        flow.identity().plane_id != metadata.plane_id ||
        flow.identity().program_epoch_id != metadata.program_epoch_id ||
        flow.identity().configuration_id != metadata.configuration_id ||
        flow.identity().physical_config_generation !=
            metadata.physical_config_generation) {
        throw OcsDataplaneError("packet_metadata_mismatch",
                                "packet metadata does not match its flow");
    }
    const auto* packet = dynamic_cast<const Packet*>(&unit);
    if (packet == nullptr || packet->size() != unit.tail_payload_bytes()) {
        throw OcsDataplaneError("packetization_mismatch",
                                "Packet::_size does not match exact tail");
    }
    if (mode_ == OcsExecutionMode::kFullPacket) {
        if (unit.logical_packet_count() != 1 ||
            unit.tail_payload_bytes() != unit.logical_payload_bytes() ||
            unit.logical_payload_bytes() > mtu_bytes_) {
            throw OcsDataplaneError("packetization_mismatch",
                                    "full-packet transit unit is malformed");
        }
    } else {
        const std::uint64_t expected_packet_count = logical_packet_count(
            unit.logical_payload_bytes(), mtu_bytes_);
        const std::uint64_t expected_tail = exact_tail_payload_bytes(
            unit.logical_payload_bytes(), mtu_bytes_);
        if (unit.logical_packet_count() != expected_packet_count ||
            unit.tail_payload_bytes() != expected_tail) {
            throw OcsDataplaneError("packetization_mismatch",
                                    "coalesced transit batch is malformed");
        }
    }
    flow.note_received(unit.first_offset(), unit.logical_payload_bytes(),
                       unit.logical_packet_count(), receive_ps);
    if (!checked_add_u64(received_payload_bytes_, unit.logical_payload_bytes(),
                         received_payload_bytes_)) {
        throw OcsDataplaneError("received_bytes_overflow",
                                "plane received byte counter overflow");
    }
}

bool OcsPlaneDataplane::data_plane_idle() const noexcept {
    if (in_flight_transit_unit_count_ != 0) {
        return false;
    }
    return std::all_of(serializers_.begin(), serializers_.end(),
                       [](const auto& serializer) {
                           return serializer->idle();
                       });
}

bool OcsPlaneDataplane::path_installed() const noexcept {
    return switch_->installed();
}

std::uint64_t OcsPlaneDataplane::serializer_backlog_flow_count() const noexcept {
    std::uint64_t total = 0;
    for (const auto& serializer : serializers_) {
        const auto count = static_cast<std::uint64_t>(
            serializer->backlog_flow_count());
        if (count > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        total += count;
    }
    return total;
}

std::uint64_t OcsPlaneDataplane::serializer_backlog_bytes() const noexcept {
    std::uint64_t total = 0;
    for (const auto& serializer : serializers_) {
        const std::uint64_t bytes = serializer->backlog_bytes();
        if (bytes > std::numeric_limits<std::uint64_t>::max() - total) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        total += bytes;
    }
    return total;
}

void OcsPlaneDataplane::abort_pending() noexcept {
    for (auto& serializer : serializers_) {
        serializer->abort_pending();
    }
    for (auto& pipe : pipes_) {
        auto* owned = dynamic_cast<OcsOwnedPipe*>(pipe.get());
        if (owned != nullptr) {
            owned->abort_inflight([this]() { transit_leave(); });
        }
    }
}

OcsPlaneDataplaneStats OcsPlaneDataplane::stats() const {
    OcsPlaneDataplaneStats result{plane_id_, 0, received_payload_bytes_, 0, 0,
                                  in_flight_transit_unit_count_, {}};
    result.source_ports.reserve(serializers_.size());
    for (const auto& serializer : serializers_) {
        OcsSourcePortStats port = serializer->stats();
        if (!checked_add_u64(result.sent_payload_bytes,
                             port.sent_payload_bytes,
                             result.sent_payload_bytes) ||
            !checked_add_u64(result.logical_packet_count,
                             port.logical_packet_count,
                             result.logical_packet_count) ||
            !checked_add_u64(result.simulated_transit_unit_count,
                             port.simulated_transit_unit_count,
                             result.simulated_transit_unit_count)) {
            throw OcsDataplaneError("plane_counter_overflow",
                                    "plane aggregate counter overflow");
        }
        result.source_ports.push_back(std::move(port));
    }
    return result;
}

bool OcsPlaneDataplane::routes_are_exact() const noexcept {
    return route_table_->has_exact_three_hop_routes();
}

std::uint64_t OcsPlaneDataplane::active_program_epoch_id() const noexcept {
    return switch_->program_epoch_id();
}

std::uint64_t OcsPlaneDataplane::active_configuration_id() const noexcept {
    return switch_->configuration_id();
}

std::uint64_t
OcsPlaneDataplane::active_physical_config_generation() const noexcept {
    return switch_->physical_config_generation();
}

}  // namespace htsim_ocs
