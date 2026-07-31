#ifndef OVERLAP4OCS_HTSIM_OCS_EXECUTION_MODE_H
#define OVERLAP4OCS_HTSIM_OCS_EXECUTION_MODE_H

#include <cstdint>
#include <string>
#include <vector>

#include "ocs_execution_plan.h"

namespace htsim_ocs {

enum class OcsExecutionMode {
    kFullPacket,
    kExactCoalesced,
};

struct OcsCapacityProof {
    OcsExecutionMode execution_mode;
    std::vector<std::vector<std::uint64_t>> per_pipe_transit_unit_upper_bound;
    std::string conservative_max_live_object_upper_bound;
};

OcsExecutionMode parse_execution_mode(const std::string& mode);
const char* execution_mode_name(OcsExecutionMode mode) noexcept;

void validate_execution_mode_preconditions(const OcsExecutionPlanV2& plan,
                                           OcsExecutionMode mode);
OcsCapacityProof prove_capacity_before_event_sources(
    const OcsExecutionPlanV2& plan, OcsExecutionMode mode);

std::uint64_t logical_packet_count(std::uint64_t payload_bytes,
                                   std::uint64_t mtu_bytes);
std::uint64_t exact_tail_payload_bytes(std::uint64_t payload_bytes,
                                       std::uint64_t mtu_bytes);
std::uint64_t cumulative_serialization_finish_ps(
    std::uint64_t busy_period_start_ps, std::uint64_t cumulative_payload_bytes,
    std::uint64_t per_plane_bps);

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_EXECUTION_MODE_H
