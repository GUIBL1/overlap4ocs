#ifndef OVERLAP4OCS_HTSIM_OCS_FLOW_H
#define OVERLAP4OCS_HTSIM_OCS_FLOW_H

#include <cstdint>
#include <functional>
#include <optional>

#include "ocs_execution_plan.h"
#include "ocs_packet_flow_bridge.h"

namespace htsim_ocs {

struct OcsFlowRuntimeIdentity {
    std::uint64_t step_id;
    std::uint64_t plane_id;
    std::uint64_t program_epoch_id;
    std::uint64_t configuration_id;
    std::uint64_t physical_config_generation;
};

struct OcsFlowStats {
    std::uint64_t flow_id;
    std::uint64_t flow_group_id;
    std::uint64_t src_rank;
    std::uint64_t dst_rank;
    std::uint64_t payload_bytes;
    std::optional<std::uint64_t> release_ps;
    std::optional<std::uint64_t> last_payload_sent_ps;
    std::optional<std::uint64_t> last_payload_received_ps;
    std::uint64_t sent_payload_bytes;
    std::uint64_t received_payload_bytes;
    std::uint64_t logical_packet_count;
    std::uint64_t tail_payload_bytes;
    bool complete;
};

using OcsFlowCompletionCallback = std::function<void(const OcsFlowStats&)>;
using OcsFlowLastSentCallback = std::function<void(const OcsFlowStats&)>;

class OcsFlow final {
  public:
    OcsFlow(const OcsFlowSpec& spec, OcsFlowRuntimeIdentity identity,
            OcsFlowCompletionCallback completion_callback = {},
            OcsFlowLastSentCallback last_sent_callback = {});

    const OcsFlowSpec& spec() const noexcept { return spec_; }
    const OcsFlowRuntimeIdentity& identity() const noexcept { return identity_; }
    OcsPacketFlowBridge& bridge() noexcept { return bridge_; }

    void mark_released(std::uint64_t release_ps);
    void note_sent(std::uint64_t byte_offset, std::uint64_t payload_bytes,
                   std::uint64_t logical_packet_count,
                   std::uint64_t tail_payload_bytes,
                   std::uint64_t last_payload_sent_ps);
    void note_received(std::uint64_t byte_offset, std::uint64_t payload_bytes,
                       std::uint64_t logical_packet_count,
                       std::uint64_t last_payload_received_ps);

    OcsFlowStats stats() const;
    std::uint64_t next_send_offset() const noexcept { return sent_payload_bytes_; }
    std::uint64_t next_receive_offset() const noexcept {
        return received_payload_bytes_;
    }
    bool complete() const noexcept { return complete_; }

  private:
    OcsFlowSpec spec_;
    OcsFlowRuntimeIdentity identity_;
    OcsPacketFlowBridge bridge_;
    OcsFlowCompletionCallback completion_callback_;
    OcsFlowLastSentCallback last_sent_callback_;
    std::optional<std::uint64_t> release_ps_;
    std::optional<std::uint64_t> last_payload_sent_ps_;
    std::optional<std::uint64_t> last_payload_received_ps_;
    std::uint64_t sent_payload_bytes_ = 0;
    std::uint64_t received_payload_bytes_ = 0;
    std::uint64_t sent_logical_packet_count_ = 0;
    std::uint64_t received_logical_packet_count_ = 0;
    std::uint64_t tail_payload_bytes_ = 0;
    bool complete_ = false;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_FLOW_H
