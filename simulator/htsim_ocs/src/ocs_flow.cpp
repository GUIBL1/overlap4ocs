#include "ocs_flow.h"

#include <utility>

#include "checked_arithmetic.h"
#include "ocs_dataplane_error.h"

namespace htsim_ocs {
namespace {

std::uint64_t checked_accumulate(std::uint64_t current, std::uint64_t increment,
                                 const char* code, const char* message) {
    std::uint64_t result = 0;
    if (!checked_add_u64(current, increment, result)) {
        throw OcsDataplaneError(code, message);
    }
    return result;
}

}  // namespace

OcsFlow::OcsFlow(const OcsFlowSpec& spec, OcsFlowRuntimeIdentity identity,
                 OcsFlowCompletionCallback completion_callback)
    : spec_(spec),
      identity_(identity),
      completion_callback_(std::move(completion_callback)) {}

void OcsFlow::mark_released(std::uint64_t release_ps) {
    if (release_ps_.has_value()) {
        throw OcsDataplaneError("duplicate_flow_release",
                                "flow was released more than once");
    }
    release_ps_ = release_ps;
}

void OcsFlow::note_sent(std::uint64_t byte_offset, std::uint64_t payload_bytes,
                        std::uint64_t logical_packet_count,
                        std::uint64_t tail_payload_bytes,
                        std::uint64_t last_payload_sent_ps) {
    if (!release_ps_.has_value() || complete_ || payload_bytes == 0 ||
        logical_packet_count == 0 || tail_payload_bytes == 0 ||
        byte_offset != sent_payload_bytes_) {
        throw OcsDataplaneError("flow_send_sequence_mismatch",
                                "flow send byte range is not contiguous");
    }
    const std::uint64_t next_bytes = checked_accumulate(
        sent_payload_bytes_, payload_bytes, "flow_sent_bytes_overflow",
        "flow sent byte counter overflow");
    if (next_bytes > spec_.payload_bytes) {
        throw OcsDataplaneError("flow_send_overflow",
                                "flow sent beyond its declared payload");
    }
    sent_payload_bytes_ = next_bytes;
    sent_logical_packet_count_ = checked_accumulate(
        sent_logical_packet_count_, logical_packet_count,
        "flow_packet_count_overflow", "flow logical packet counter overflow");
    tail_payload_bytes_ = tail_payload_bytes;
    last_payload_sent_ps_ = last_payload_sent_ps;
}

void OcsFlow::note_received(std::uint64_t byte_offset,
                            std::uint64_t payload_bytes,
                            std::uint64_t logical_packet_count,
                            std::uint64_t last_payload_received_ps) {
    if (!release_ps_.has_value() || complete_ || payload_bytes == 0 ||
        logical_packet_count == 0 || byte_offset != received_payload_bytes_) {
        throw OcsDataplaneError("flow_receive_sequence_mismatch",
                                "flow receive byte range is not contiguous");
    }
    const std::uint64_t next_bytes = checked_accumulate(
        received_payload_bytes_, payload_bytes, "flow_received_bytes_overflow",
        "flow received byte counter overflow");
    if (next_bytes > spec_.payload_bytes || next_bytes > sent_payload_bytes_) {
        throw OcsDataplaneError("flow_receive_overflow",
                                "flow received bytes outside its sent range");
    }
    received_payload_bytes_ = next_bytes;
    received_logical_packet_count_ = checked_accumulate(
        received_logical_packet_count_, logical_packet_count,
        "flow_packet_count_overflow", "flow logical packet counter overflow");
    last_payload_received_ps_ = last_payload_received_ps;

    if (received_payload_bytes_ == spec_.payload_bytes) {
        if (sent_payload_bytes_ != spec_.payload_bytes ||
            received_logical_packet_count_ != sent_logical_packet_count_) {
            throw OcsDataplaneError("flow_completion_counter_mismatch",
                                    "flow completion counters do not match");
        }
        complete_ = true;
        if (completion_callback_) {
            completion_callback_(stats());
        }
    }
}

OcsFlowStats OcsFlow::stats() const {
    return OcsFlowStats{spec_.flow_id,
                        spec_.flow_group_id,
                        spec_.src_rank,
                        spec_.dst_rank,
                        spec_.payload_bytes,
                        release_ps_,
                        last_payload_sent_ps_,
                        last_payload_received_ps_,
                        sent_payload_bytes_,
                        received_payload_bytes_,
                        sent_logical_packet_count_,
                        tail_payload_bytes_,
                        complete_};
}

}  // namespace htsim_ocs
