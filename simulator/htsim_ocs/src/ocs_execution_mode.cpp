#include "ocs_execution_mode.h"

#include <algorithm>
#include <limits>
#include <sstream>

#include "checked_arithmetic.h"
#include "generated_contract.h"
#include "ocs_dataplane_error.h"

namespace htsim_ocs {
namespace {

std::string u128_decimal(uint128_t value) {
    if (value == 0) {
        return "0";
    }
    std::string digits;
    while (value != 0) {
        digits.push_back(static_cast<char>('0' + value % 10));
        value /= 10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

[[noreturn]] void unsupported(const char* code, const char* message) {
    throw OcsDataplaneError(code, message,
                            ExitCode::kInvalidOrUnsupported);
}

}  // namespace

OcsExecutionMode parse_execution_mode(const std::string& mode) {
    if (mode == "full_packet") {
        return OcsExecutionMode::kFullPacket;
    }
    if (mode == "exact_coalesced") {
        return OcsExecutionMode::kExactCoalesced;
    }
    unsupported("unsupported_execution_mode", "execution mode is unsupported");
}

const char* execution_mode_name(OcsExecutionMode mode) noexcept {
    return mode == OcsExecutionMode::kFullPacket ? "full_packet"
                                                  : "exact_coalesced";
}

std::uint64_t logical_packet_count(std::uint64_t payload_bytes,
                                   std::uint64_t mtu_bytes) {
    if (payload_bytes == 0 || mtu_bytes == 0) {
        throw OcsDataplaneError("packetization_error",
                                "payload and MTU must be positive");
    }
    return 1 + (payload_bytes - 1) / mtu_bytes;
}

std::uint64_t exact_tail_payload_bytes(std::uint64_t payload_bytes,
                                       std::uint64_t mtu_bytes) {
    (void)logical_packet_count(payload_bytes, mtu_bytes);
    const std::uint64_t remainder = payload_bytes % mtu_bytes;
    return remainder == 0 ? mtu_bytes : remainder;
}

std::uint64_t cumulative_serialization_finish_ps(
    std::uint64_t busy_period_start_ps, std::uint64_t cumulative_payload_bytes,
    std::uint64_t per_plane_bps) {
    if (cumulative_payload_bytes == 0 || per_plane_bps == 0) {
        throw OcsDataplaneError("serialization_time_error",
                                "serialization inputs must be positive");
    }
    constexpr uint128_t kBitsPerByte = 8;
    constexpr uint128_t kPicosecondsPerSecond = 1000000000000ULL;
    const uint128_t numerator = static_cast<uint128_t>(cumulative_payload_bytes) *
                                kBitsPerByte * kPicosecondsPerSecond;
    const uint128_t duration =
        (numerator + static_cast<uint128_t>(per_plane_bps) - 1) /
        static_cast<uint128_t>(per_plane_bps);
    const uint128_t finish = static_cast<uint128_t>(busy_period_start_ps) +
                             duration;
    std::uint64_t result = 0;
    if (!u128_to_u64(finish, result)) {
        throw OcsDataplaneError("simulation_time_overflow",
                                "serialization finish time exceeds uint64");
    }
    return result;
}

void validate_execution_mode_preconditions(const OcsExecutionPlanV2& plan,
                                           OcsExecutionMode mode) {
    if (plan.transport.mode != "paper_exact" ||
        plan.transport.wire_model != "payload_only" ||
        plan.transport.packetization != "exact_tail" ||
        plan.transport.loss_mode != "lossless" ||
        plan.transport.ack_mode != "none" ||
        plan.transport.completion_semantics !=
            "receiver_last_payload_byte" ||
        plan.topology.duplex != "full") {
        unsupported("unsupported_execution_mode",
                    "transport does not satisfy exact data-plane preconditions");
    }
    if (plan.transport.mtu_bytes < contract::kMinMtuBytes ||
        plan.transport.mtu_bytes > contract::kMaxMtuBytes) {
        unsupported("unsupported_execution_mode",
                    "MTU is outside the packet data-plane ABI");
    }
    if (plan.topology.node_count >
        std::numeric_limits<std::uint32_t>::max()) {
        unsupported("unsupported_execution_mode",
                    "HTSim packet destination cannot represent node_count");
    }
    if (plan.execution_mode != execution_mode_name(mode)) {
        unsupported("unsupported_execution_mode",
                    "requested execution mode differs from the plan");
    }
}

OcsCapacityProof prove_capacity_before_event_sources(
    const OcsExecutionPlanV2& plan, OcsExecutionMode mode) {
    validate_execution_mode_preconditions(plan, mode);
    std::vector<std::vector<uint128_t>> counts(
        static_cast<std::size_t>(plan.topology.plane_count),
        std::vector<uint128_t>(
            static_cast<std::size_t>(plan.topology.node_count), 0));

    for (const OcsFlowSpec& flow : plan.flows) {
        const OcsFlowGroupSpec& group =
            plan.flow_group_by_id(flow.flow_group_id);
        const std::uint64_t units =
            mode == OcsExecutionMode::kFullPacket
                ? logical_packet_count(flow.payload_bytes,
                                       plan.transport.mtu_bytes)
                : 1;
        uint128_t& pipe_count =
            counts[static_cast<std::size_t>(group.plane_id)]
                  [static_cast<std::size_t>(flow.dst_rank)];
        pipe_count += units;
        if (pipe_count > contract::kMaxPipeInflightTransitUnits) {
            unsupported("upstream_pipe_capacity_limit",
                        "conservative per-Pipe transit-unit bound exceeds ABI limit");
        }
    }

    OcsCapacityProof proof;
    proof.execution_mode = mode;
    proof.per_pipe_transit_unit_upper_bound.resize(counts.size());
    uint128_t total_pipe_units = 0;
    for (std::size_t plane = 0; plane < counts.size(); ++plane) {
        proof.per_pipe_transit_unit_upper_bound[plane].reserve(
            counts[plane].size());
        for (uint128_t count : counts[plane]) {
            total_pipe_units += count;
            proof.per_pipe_transit_unit_upper_bound[plane].push_back(
                static_cast<std::uint64_t>(count));
        }
    }
    const uint128_t serializer_current_upper_bound =
        static_cast<uint128_t>(plan.topology.plane_count) *
        static_cast<uint128_t>(plan.topology.node_count);
    proof.conservative_max_live_object_upper_bound =
        u128_decimal(total_pipe_units + serializer_current_upper_bound);
    return proof;
}

}  // namespace htsim_ocs
