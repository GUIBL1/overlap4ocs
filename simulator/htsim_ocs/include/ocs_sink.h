#ifndef OVERLAP4OCS_HTSIM_OCS_SINK_H
#define OVERLAP4OCS_HTSIM_OCS_SINK_H

#include <cstdint>
#include <functional>
#include <string>

#include "network.h"

namespace htsim_ocs {

class OcsTransitUnitView;

class OcsSink final : public PacketSink {
  public:
    using ReceiveCallback =
        std::function<void(const OcsTransitUnitView&, std::uint64_t)>;
    using TransitLeaveCallback = std::function<void()>;

    OcsSink(std::uint64_t plane_id, std::uint64_t dst_rank,
            ReceiveCallback receive_callback,
            TransitLeaveCallback transit_leave_callback);

    void receivePacket(Packet& packet) override;
    const std::string& nodename() override { return name_; }

  private:
    std::uint64_t plane_id_;
    std::uint64_t dst_rank_;
    ReceiveCallback receive_callback_;
    TransitLeaveCallback transit_leave_callback_;
    std::string name_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_SINK_H
