#include "ocs_switch.h"

#include <limits>
#include <sstream>
#include <utility>

#include "checked_arithmetic.h"
#include "eventlist.h"
#include "ocs_dataplane_error.h"
#include "ocs_packet.h"

namespace htsim_ocs {

OcsSwitch::OcsSwitch(std::uint64_t plane_id, std::uint64_t data_latency_ps,
                     TransitEnterCallback transit_enter_callback,
                     TransitRollbackCallback transit_rollback_callback)
    : plane_id_(plane_id),
      data_latency_ps_(data_latency_ps),
      transit_enter_callback_(std::move(transit_enter_callback)),
      transit_rollback_callback_(std::move(transit_rollback_callback)) {
    std::ostringstream stream;
    stream << "ocs-switch-plane-" << plane_id;
    name_ = stream.str();
}

void OcsSwitch::install(std::uint64_t program_epoch_id,
                        std::uint64_t configuration_id,
                        std::uint64_t physical_config_generation,
                        std::vector<std::uint64_t> permutation) {
    if (permutation.empty()) {
        throw OcsDataplaneError("configuration_not_installed",
                                "cannot install an empty permutation");
    }
    program_epoch_id_ = program_epoch_id;
    configuration_id_ = configuration_id;
    physical_config_generation_ = physical_config_generation;
    permutation_ = std::move(permutation);
    installed_ = true;
}

[[noreturn]] void OcsSwitch::reject(Packet& packet, const char* error_code,
                                    const char* message) const {
    if (auto* unit = dynamic_cast<OcsTransitUnitView*>(&packet);
        unit != nullptr && unit->pool_active()) {
        packet.free();
    }
    throw OcsDataplaneError(error_code, message);
}

void OcsSwitch::receivePacket(Packet& packet) {
    auto* unit = dynamic_cast<OcsTransitUnitView*>(&packet);
    if (unit == nullptr) {
        reject(packet, "unexpected_packet_type",
               "OCS switch received a non-OCS packet");
    }
    const OcsPacketMetadata& metadata = unit->metadata();
    if (!installed_) {
        reject(packet, "configuration_not_installed",
               "OCS switch has no installed configuration");
    }
    if (metadata.plane_id != plane_id_) {
        reject(packet, "packet_plane_mismatch",
               "packet plane does not match switch plane");
    }
    if (metadata.program_epoch_id != program_epoch_id_) {
        reject(packet, "stale_program_epoch",
               "packet program epoch is not active");
    }
    if (metadata.configuration_id != configuration_id_) {
        reject(packet, "route_mismatch",
               "packet configuration is not active");
    }
    if (metadata.physical_config_generation != physical_config_generation_) {
        reject(packet, "stale_physical_generation",
               "packet physical configuration generation is stale");
    }
    if (metadata.src_rank >= permutation_.size() ||
        metadata.dst_rank >= permutation_.size() ||
        permutation_[static_cast<std::size_t>(metadata.src_rank)] !=
            metadata.dst_rank) {
        reject(packet, "route_mismatch",
               "packet destination does not match active permutation");
    }
    std::uint64_t arrival_ps = 0;
    if (!checked_add_u64(EventList::now(), data_latency_ps_, arrival_ps)) {
        reject(packet, "simulation_time_overflow",
               "Pipe arrival time exceeds uint64");
    }
    (void)arrival_ps;
    if (transit_enter_callback_) {
        transit_enter_callback_();
    }
    try {
        packet.sendOn();
    } catch (...) {
        if (transit_rollback_callback_) {
            transit_rollback_callback_();
        }
        if (unit->pool_active()) {
            packet.free();
        }
        throw;
    }
}

}  // namespace htsim_ocs
