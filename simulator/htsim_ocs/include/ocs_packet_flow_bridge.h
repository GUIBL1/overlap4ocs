#ifndef OVERLAP4OCS_HTSIM_OCS_PACKET_FLOW_BRIDGE_H
#define OVERLAP4OCS_HTSIM_OCS_PACKET_FLOW_BRIDGE_H

#include "network.h"

namespace htsim_ocs {

// The bridge exists only because HTSim Pipe calls PacketFlow::logTraffic().
// Its 32-bit logging ID is deliberately never used as an OCS flow identity.
class OcsPacketFlowBridge final {
  public:
    OcsPacketFlowBridge();

    PacketFlow& packet_flow() noexcept { return packet_flow_; }
    const PacketFlow& packet_flow() const noexcept { return packet_flow_; }

  private:
    PacketFlow packet_flow_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PACKET_FLOW_BRIDGE_H
