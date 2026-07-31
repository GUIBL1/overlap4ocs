#ifndef OVERLAP4OCS_HTSIM_OCS_PLANE_DATAPLANE_H
#define OVERLAP4OCS_HTSIM_OCS_PLANE_DATAPLANE_H

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "ocs_execution_mode.h"
#include "ocs_packet.h"
#include "ocs_port_serializer.h"

class EventList;
class Pipe;

namespace htsim_ocs {

class OcsFlow;
class OcsRouteTable;
class OcsSink;
class OcsSwitch;

struct OcsPlaneDataplaneStats {
    std::uint64_t plane_id;
    std::uint64_t sent_payload_bytes;
    std::uint64_t received_payload_bytes;
    std::uint64_t logical_packet_count;
    std::uint64_t simulated_transit_unit_count;
    std::uint64_t in_flight_transit_unit_count;
    std::vector<OcsSourcePortStats> source_ports;
};

class OcsPlaneDataplane final {
  public:
    using FlowLookup = std::function<OcsFlow&(std::uint64_t)>;

    OcsPlaneDataplane(EventList& event_list, std::uint64_t plane_id,
                      std::uint64_t node_count, std::uint64_t per_plane_bps,
                      std::uint64_t data_latency_ps, std::uint64_t mtu_bytes,
                      OcsExecutionMode mode,
                      OcsPacketPool<OcsPacket>& packet_pool,
                      OcsPacketPool<OcsTransitBatch>& batch_pool,
                      FlowLookup flow_lookup);
    ~OcsPlaneDataplane();

    void activate_static_path(std::uint64_t program_epoch_id,
                              std::uint64_t configuration_id,
                              std::uint64_t physical_config_generation,
                              const std::vector<std::uint64_t>& permutation);
    void enqueue_flows(std::vector<OcsFlow*> flows);

    bool data_plane_idle() const noexcept;
    std::uint64_t in_flight_transit_unit_count() const noexcept {
        return in_flight_transit_unit_count_;
    }
    OcsPlaneDataplaneStats stats() const;
    bool routes_are_exact() const noexcept;

    std::uint64_t active_program_epoch_id() const noexcept;
    std::uint64_t active_configuration_id() const noexcept;
    std::uint64_t active_physical_config_generation() const noexcept;

  private:
    void transit_enter();
    void transit_leave();
    void receive_transit_unit(const OcsTransitUnitView& unit,
                              std::uint64_t receive_ps);

    std::uint64_t plane_id_;
    std::uint64_t node_count_;
    std::uint64_t mtu_bytes_;
    OcsExecutionMode mode_;
    FlowLookup flow_lookup_;
    std::uint64_t in_flight_transit_unit_count_ = 0;
    std::uint64_t received_payload_bytes_ = 0;
    std::unique_ptr<OcsSwitch> switch_;
    std::vector<std::unique_ptr<Pipe>> pipes_;
    std::vector<std::unique_ptr<OcsSink>> sinks_;
    std::unique_ptr<OcsRouteTable> route_table_;
    std::vector<std::unique_ptr<OcsPortSerializer>> serializers_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PLANE_DATAPLANE_H
