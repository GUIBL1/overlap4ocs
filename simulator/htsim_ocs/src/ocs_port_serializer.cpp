#include "ocs_port_serializer.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include "checked_arithmetic.h"
#include "ocs_dataplane_error.h"
#include "ocs_route_table.h"

namespace htsim_ocs {
namespace {

std::string serializer_name(std::uint64_t plane_id, std::uint64_t src_rank) {
    std::ostringstream stream;
    stream << "ocs-serializer-plane-" << plane_id << "-src-" << src_rank;
    return stream.str();
}

std::uint64_t checked_counter_add(std::uint64_t current,
                                  std::uint64_t increment,
                                  const char* error_code) {
    std::uint64_t result = 0;
    if (!checked_add_u64(current, increment, result)) {
        throw OcsDataplaneError(error_code, "serializer counter overflow");
    }
    return result;
}

}  // namespace

OcsPortSerializer::OcsPortSerializer(
    EventList& event_list, std::uint64_t plane_id, std::uint64_t src_rank,
    std::uint64_t per_plane_bps, std::uint64_t mtu_bytes,
    OcsExecutionMode mode, OcsPacketPool<OcsPacket>& packet_pool,
    OcsPacketPool<OcsTransitBatch>& batch_pool,
    const OcsRouteTable& route_table)
    : EventSource(event_list, serializer_name(plane_id, src_rank)),
      plane_id_(plane_id),
      src_rank_(src_rank),
      per_plane_bps_(per_plane_bps),
      mtu_bytes_(mtu_bytes),
      mode_(mode),
      packet_pool_(packet_pool),
      batch_pool_(batch_pool),
      route_table_(route_table) {}

void OcsPortSerializer::enqueue_flows(std::vector<OcsFlow*> flows) {
    std::sort(flows.begin(), flows.end(), [](const OcsFlow* left,
                                             const OcsFlow* right) {
        return left->spec().flow_id < right->spec().flow_id;
    });
    for (OcsFlow* flow : flows) {
        if (flow == nullptr || flow->identity().plane_id != plane_id_ ||
            flow->spec().src_rank != src_rank_) {
            throw OcsDataplaneError("serializer_port_mismatch",
                                    "flow was queued on the wrong serializer");
        }
        if (flow->stats().release_ps.has_value()) {
            throw OcsDataplaneError("duplicate_flow_release",
                                    "flow was queued more than once");
        }
        flow->mark_released(EventList::now());
        queue_.push_back(flow);
        backlog_bytes_ = checked_counter_add(
            backlog_bytes_, flow->spec().payload_bytes,
            "serializer_backlog_overflow");
    }
    if (backlog_bytes_ > max_backlog_bytes_) {
        max_backlog_bytes_ = backlog_bytes_;
    }
    start_or_continue_busy_period();
}

void OcsPortSerializer::start_or_continue_busy_period() {
    if (event_pending_ || queue_.empty()) {
        return;
    }
    if (!busy_start_ps_.has_value()) {
        busy_start_ps_ = EventList::now();
        cumulative_busy_payload_bytes_ = 0;
    }
    schedule_next_unit();
}

void OcsPortSerializer::schedule_next_unit() {
    if (queue_.empty() || !busy_start_ps_.has_value() || event_pending_) {
        return;
    }
    OcsFlow* flow = queue_.front();
    const std::uint64_t byte_offset = flow->next_send_offset();
    if (byte_offset >= flow->spec().payload_bytes) {
        throw OcsDataplaneError("flow_send_sequence_mismatch",
                                "serializer found an exhausted queued flow");
    }
    const std::uint64_t remaining = flow->spec().payload_bytes - byte_offset;
    std::uint64_t payload = 0;
    std::uint64_t packet_count = 0;
    std::uint64_t tail = 0;
    if (mode_ == OcsExecutionMode::kFullPacket) {
        payload = std::min(mtu_bytes_, remaining);
        packet_count = 1;
        tail = payload;
    } else {
        payload = remaining;
        packet_count = logical_packet_count(remaining, mtu_bytes_);
        tail = exact_tail_payload_bytes(remaining, mtu_bytes_);
    }
    cumulative_busy_payload_bytes_ = checked_counter_add(
        cumulative_busy_payload_bytes_, payload,
        "serializer_cumulative_bytes_overflow");
    const std::uint64_t finish_ps = cumulative_serialization_finish_ps(
        *busy_start_ps_, cumulative_busy_payload_bytes_, per_plane_bps_);
    pending_unit_ = PendingUnit{flow,
                                byte_offset,
                                payload,
                                packet_count,
                                tail,
                                byte_offset / mtu_bytes_,
                                finish_ps};
    event_pending_ = true;
    EventList::sourceIsPending(*this, finish_ps);
}

void OcsPortSerializer::emit_pending_unit() {
    if (!pending_unit_.has_value() || !event_pending_) {
        throw OcsDataplaneError("serializer_event_mismatch",
                                "serializer callback has no pending unit");
    }
    PendingUnit unit = *pending_unit_;
    pending_unit_.reset();
    event_pending_ = false;
    if (EventList::now() != unit.finish_ps || queue_.empty() ||
        queue_.front() != unit.flow) {
        throw OcsDataplaneError("serializer_event_mismatch",
                                "serializer callback state is inconsistent");
    }

    OcsPacketMetadata metadata{unit.flow->spec().flow_id,
                               unit.flow->spec().flow_group_id,
                               unit.flow->identity().step_id,
                               unit.flow->identity().plane_id,
                               unit.flow->identity().program_epoch_id,
                               unit.flow->identity().configuration_id,
                               unit.flow->identity().physical_config_generation,
                               unit.flow->spec().src_rank,
                               unit.flow->spec().dst_rank,
                               unit.byte_offset,
                               unit.logical_packet_ordinal};
    Packet* packet = nullptr;
    try {
        const Route& route = route_table_.route_for_destination(
            unit.flow->spec().dst_rank);
        if (mode_ == OcsExecutionMode::kFullPacket) {
            auto* concrete = packet_pool_.allocate();
            packet = concrete;
            concrete->initialize(
                unit.flow->bridge().packet_flow(), route, metadata,
                static_cast<std::uint16_t>(unit.tail_payload_bytes),
                unit.finish_ps);
        } else {
            auto* concrete = batch_pool_.allocate();
            packet = concrete;
            concrete->initialize(
                unit.flow->bridge().packet_flow(), route, metadata,
                unit.logical_payload_bytes, unit.logical_packet_count,
                static_cast<std::uint16_t>(unit.tail_payload_bytes),
                unit.finish_ps);
        }
        unit.flow->note_sent(unit.byte_offset, unit.logical_payload_bytes,
                             unit.logical_packet_count,
                             unit.tail_payload_bytes, unit.finish_ps);
        backlog_bytes_ -= unit.logical_payload_bytes;
        sent_payload_bytes_ = checked_counter_add(
            sent_payload_bytes_, unit.logical_payload_bytes,
            "serializer_sent_bytes_overflow");
        logical_packet_count_ = checked_counter_add(
            logical_packet_count_, unit.logical_packet_count,
            "serializer_packet_count_overflow");
        simulated_transit_unit_count_ = checked_counter_add(
            simulated_transit_unit_count_, 1,
            "serializer_transit_unit_count_overflow");
        packet->sendOn();
    } catch (...) {
        auto* view = dynamic_cast<OcsTransitUnitView*>(packet);
        if (packet != nullptr && view != nullptr && view->pool_active()) {
            packet->free();
        }
        throw;
    }

    if (unit.flow->next_send_offset() == unit.flow->spec().payload_bytes) {
        queue_.pop_front();
    }
    if (queue_.empty()) {
        close_busy_interval();
    } else {
        schedule_next_unit();
    }
}

void OcsPortSerializer::close_busy_interval() {
    if (!busy_start_ps_.has_value() || backlog_bytes_ != 0) {
        throw OcsDataplaneError("serializer_drain_mismatch",
                                "serializer busy interval cannot close");
    }
    const std::uint64_t end_ps = EventList::now();
    if (end_ps < *busy_start_ps_) {
        throw OcsDataplaneError("serializer_time_mismatch",
                                "serializer time moved backwards");
    }
    busy_intervals_.push_back(OcsBusyInterval{*busy_start_ps_, end_ps});
    busy_time_ps_ = checked_counter_add(
        busy_time_ps_, end_ps - *busy_start_ps_,
        "serializer_busy_time_overflow");
    busy_start_ps_.reset();
    cumulative_busy_payload_bytes_ = 0;
}

void OcsPortSerializer::doNextEvent() { emit_pending_unit(); }

OcsSourcePortStats OcsPortSerializer::stats() const {
    return OcsSourcePortStats{plane_id_,
                              src_rank_,
                              sent_payload_bytes_,
                              logical_packet_count_,
                              simulated_transit_unit_count_,
                              max_backlog_bytes_,
                              busy_time_ps_,
                              busy_intervals_};
}

}  // namespace htsim_ocs
