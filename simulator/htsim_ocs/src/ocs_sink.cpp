#include "ocs_sink.h"

#include <sstream>
#include <utility>

#include "eventlist.h"
#include "ocs_dataplane_error.h"
#include "ocs_packet.h"

namespace htsim_ocs {

OcsSink::OcsSink(std::uint64_t plane_id, std::uint64_t dst_rank,
                 ReceiveCallback receive_callback,
                 TransitLeaveCallback transit_leave_callback)
    : plane_id_(plane_id),
      dst_rank_(dst_rank),
      receive_callback_(std::move(receive_callback)),
      transit_leave_callback_(std::move(transit_leave_callback)) {
    std::ostringstream stream;
    stream << "ocs-sink-plane-" << plane_id << "-dst-" << dst_rank;
    name_ = stream.str();
}

void OcsSink::receivePacket(Packet& packet) {
    auto* unit = dynamic_cast<OcsTransitUnitView*>(&packet);
    if (unit == nullptr) {
        if (transit_leave_callback_) {
            transit_leave_callback_();
        }
        packet.free();
        throw OcsDataplaneError("unexpected_packet_type",
                                "OCS sink received a non-OCS packet");
    }

    try {
        const OcsPacketMetadata& metadata = unit->metadata();
        if (metadata.plane_id != plane_id_) {
            throw OcsDataplaneError("packet_plane_mismatch",
                                    "packet plane does not match sink plane");
        }
        if (metadata.dst_rank != dst_rank_) {
            throw OcsDataplaneError("route_mismatch",
                                    "packet arrived at the wrong sink");
        }
        if (receive_callback_) {
            receive_callback_(*unit, EventList::now());
        }
    } catch (...) {
        if (transit_leave_callback_) {
            transit_leave_callback_();
        }
        if (unit->pool_active()) {
            packet.free();
        }
        throw;
    }

    if (transit_leave_callback_) {
        transit_leave_callback_();
    }
    packet.free();
}

}  // namespace htsim_ocs
