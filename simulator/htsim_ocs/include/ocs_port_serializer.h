#ifndef OVERLAP4OCS_HTSIM_OCS_PORT_SERIALIZER_H
#define OVERLAP4OCS_HTSIM_OCS_PORT_SERIALIZER_H

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "eventlist.h"
#include "ocs_execution_mode.h"
#include "ocs_flow.h"
#include "ocs_packet.h"

namespace htsim_ocs {

class OcsRouteTable;

struct OcsBusyInterval {
    std::uint64_t start_ps;
    std::uint64_t end_ps;
    bool truncated_at_stop;
};

struct OcsSourcePortStats {
    std::uint64_t plane_id;
    std::uint64_t src_rank;
    std::uint64_t sent_payload_bytes;
    std::uint64_t logical_packet_count;
    std::uint64_t simulated_transit_unit_count;
    std::uint64_t max_backlog_bytes;
    std::uint64_t busy_time_ps;
    std::vector<OcsBusyInterval> busy_intervals;
    std::vector<std::uint64_t> max_backlog_bytes_by_program_epoch;
};

class OcsPortSerializer final : public EventSource {
  public:
    OcsPortSerializer(EventList& event_list, std::uint64_t plane_id,
                      std::uint64_t src_rank, std::uint64_t per_plane_bps,
                      std::uint64_t mtu_bytes, OcsExecutionMode mode,
                      OcsPacketPool<OcsPacket>& packet_pool,
                      OcsPacketPool<OcsTransitBatch>& batch_pool,
                      const OcsRouteTable& route_table);

    void enqueue_flows(std::vector<OcsFlow*> flows);
    void doNextEvent() override;
    void abort_pending() noexcept;

    bool idle() const noexcept { return !event_pending_ && queue_.empty(); }
    std::uint64_t backlog_bytes() const noexcept { return backlog_bytes_; }
    std::size_t backlog_flow_count() const noexcept { return queue_.size(); }
    OcsSourcePortStats stats() const;

  private:
    struct PendingUnit {
        OcsFlow* flow;
        std::uint64_t byte_offset;
        std::uint64_t logical_payload_bytes;
        std::uint64_t logical_packet_count;
        std::uint64_t tail_payload_bytes;
        std::uint64_t logical_packet_ordinal;
        std::uint64_t finish_ps;
    };

    void start_or_continue_busy_period();
    void schedule_next_unit();
    void emit_pending_unit();
    void close_busy_interval();

    std::uint64_t plane_id_;
    std::uint64_t src_rank_;
    std::uint64_t per_plane_bps_;
    std::uint64_t mtu_bytes_;
    OcsExecutionMode mode_;
    OcsPacketPool<OcsPacket>& packet_pool_;
    OcsPacketPool<OcsTransitBatch>& batch_pool_;
    const OcsRouteTable& route_table_;
    std::deque<OcsFlow*> queue_;
    std::optional<PendingUnit> pending_unit_;
    bool event_pending_ = false;
    std::optional<std::uint64_t> busy_start_ps_;
    std::uint64_t cumulative_busy_payload_bytes_ = 0;
    std::uint64_t backlog_bytes_ = 0;
    std::uint64_t max_backlog_bytes_ = 0;
    std::uint64_t sent_payload_bytes_ = 0;
    std::uint64_t logical_packet_count_ = 0;
    std::uint64_t simulated_transit_unit_count_ = 0;
    std::uint64_t busy_time_ps_ = 0;
    std::vector<OcsBusyInterval> busy_intervals_;
    std::vector<std::uint64_t> max_backlog_bytes_by_program_epoch_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PORT_SERIALIZER_H
