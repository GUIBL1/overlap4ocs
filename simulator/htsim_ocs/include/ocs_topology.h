#ifndef OVERLAP4OCS_HTSIM_OCS_TOPOLOGY_H
#define OVERLAP4OCS_HTSIM_OCS_TOPOLOGY_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ocs_execution_mode.h"
#include "ocs_flow.h"
#include "ocs_packet.h"
#include "ocs_plane_dataplane.h"

class EventList;

namespace htsim_ocs {

struct OcsStaticDataplaneAudit {
    std::string execution_mode;
    std::uint64_t expected_flow_count;
    std::uint64_t completed_flow_count;
    std::uint64_t expected_payload_bytes;
    std::uint64_t sent_payload_bytes;
    std::uint64_t received_payload_bytes;
    std::uint64_t logical_packet_count;
    std::uint64_t simulated_transit_unit_count;
    std::uint64_t processed_event_count;
    std::uint64_t in_flight_transit_unit_count;
    std::uint64_t collective_complete_ps;
    std::uint64_t packet_pool_allocated_count;
    std::uint64_t packet_pool_peak_in_use;
    std::uint64_t batch_pool_allocated_count;
    std::uint64_t batch_pool_peak_in_use;
    bool all_routes_exact;
    bool all_pools_returned;
    std::vector<std::uint64_t> per_rank_sent_payload_bytes;
    std::vector<std::uint64_t> per_rank_received_payload_bytes;
    std::vector<std::uint64_t> per_plane_payload_bytes;
    std::vector<OcsFlowStats> flows;
    std::vector<OcsPlaneDataplaneStats> planes;
};

class OcsTopology final {
  public:
    using FlowCallback = std::function<void(const OcsFlowStats&)>;

    OcsTopology(EventList& event_list,
                std::shared_ptr<const OcsExecutionPlanV2> plan,
                OcsExecutionMode mode, OcsCapacityProof capacity_proof,
                FlowCallback flow_completion_callback = {},
                FlowCallback flow_last_sent_callback = {});
    ~OcsTopology();

    void release_static_groups(std::vector<std::uint64_t> flow_group_ids);
    void install_epoch_path(std::uint64_t plane_id,
                            std::uint64_t program_epoch_id);
    void release_group(std::uint64_t flow_group_id);
    void abort_pending();
    void run_until_idle();
    bool data_plane_idle() const noexcept;
    bool plane_data_plane_idle(std::uint64_t plane_id) const;
    OcsStaticDataplaneAudit audit() const;
    std::vector<OcsFlowStats> all_flow_stats() const;

    OcsFlow& flow_by_id(std::uint64_t flow_id);
    OcsPlaneDataplane& plane_by_id(std::uint64_t plane_id);
    const OcsPlaneDataplane& plane_by_id(std::uint64_t plane_id) const;
    const OcsCapacityProof& capacity_proof() const noexcept {
        return capacity_proof_;
    }

  private:
    std::uint64_t physical_generation_for_epoch(
        std::uint64_t plane_id, std::uint64_t program_epoch_id) const;
    void materialize_all_flows();
    void materialize_group(const OcsFlowGroupSpec& group);
    void note_flow_last_sent(const OcsFlowStats& stats);
    void note_flow_complete(const OcsFlowStats& stats);

    EventList& event_list_;
    std::shared_ptr<const OcsExecutionPlanV2> plan_;
    OcsExecutionMode mode_;
    OcsCapacityProof capacity_proof_;
    OcsPacketPool<OcsPacket> packet_pool_;
    OcsPacketPool<OcsTransitBatch> batch_pool_;
    std::vector<std::unique_ptr<OcsFlow>> flows_;
    std::vector<bool> selected_flows_;
    std::vector<bool> completed_flows_;
    std::vector<std::unique_ptr<OcsPlaneDataplane>> planes_;
    FlowCallback flow_completion_callback_;
    FlowCallback flow_last_sent_callback_;
    std::uint64_t selected_flow_count_ = 0;
    std::uint64_t completed_flow_count_ = 0;
    std::uint64_t selected_payload_bytes_ = 0;
    std::uint64_t processed_event_count_at_start_ = 0;
};

OcsStaticDataplaneAudit run_static_dataplane(
    std::shared_ptr<const OcsExecutionPlanV2> plan,
    const std::vector<std::uint64_t>& flow_group_ids,
    const std::string& requested_execution_mode);

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_TOPOLOGY_H
