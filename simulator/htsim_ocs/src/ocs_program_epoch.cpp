#include "ocs_program_epoch.h"

#include "ocs_dataplane_error.h"

namespace htsim_ocs {

OcsProgramEpochRuntime::OcsProgramEpochRuntime(
    const OcsProgramEpochSpec& spec,
    std::uint64_t physical_config_generation)
    : spec_(spec),
      stats_{spec.program_epoch_id,
             spec.transition,
             spec.configuration_id,
             physical_config_generation,
             spec.path_prep_not_before_token_ids,
             spec.flow_group_ids,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             std::nullopt,
             "pending"} {}

void OcsProgramEpochRuntime::begin_path_preparation(std::uint64_t time_ps) {
    if (path_preparation_started() || closed()) {
        throw OcsDataplaneError("epoch_violation",
                                "epoch path preparation started twice");
    }
    stats_.path_prep_start_ps = time_ps;
    stats_.status = "path_preparing";
    if (spec_.transition == "reconfigure") {
        stats_.reconfiguration_start_ps = time_ps;
    }
}

void OcsProgramEpochRuntime::mark_path_ready(std::uint64_t time_ps) {
    if (!stats_.path_prep_start_ps.has_value() || path_ready() ||
        time_ps < *stats_.path_prep_start_ps) {
        throw OcsDataplaneError("epoch_violation",
                                "epoch path-ready transition is invalid");
    }
    stats_.path_ready_ps = time_ps;
    if (spec_.transition == "reconfigure") {
        stats_.reconfiguration_end_ps = time_ps;
    }
    stats_.status = "path_ready";
}

void OcsProgramEpochRuntime::note_transfer_start(std::uint64_t time_ps) {
    if (!path_ready() || closed() || time_ps < *stats_.path_ready_ps) {
        throw OcsDataplaneError("epoch_violation",
                                "epoch transfer started before its path");
    }
    if (!stats_.transfer_start_ps.has_value()) {
        stats_.transfer_start_ps = time_ps;
        stats_.status = "active";
    }
}

void OcsProgramEpochRuntime::mark_draining() {
    if (!path_ready() || closed()) {
        throw OcsDataplaneError("epoch_violation",
                                "epoch cannot enter draining state");
    }
    stats_.status = "draining";
}

void OcsProgramEpochRuntime::close(std::uint64_t transfer_complete_ps,
                                   std::uint64_t time_ps) {
    if (!path_ready() || closed() || !stats_.transfer_start_ps.has_value() ||
        transfer_complete_ps < *stats_.transfer_start_ps ||
        time_ps < transfer_complete_ps) {
        throw OcsDataplaneError("epoch_violation",
                                "epoch close violated its lifecycle");
    }
    stats_.transfer_complete_ps = transfer_complete_ps;
    stats_.drain_complete_ps = time_ps;
    stats_.close_ps = time_ps;
    stats_.status = "closed";
}

}  // namespace htsim_ocs
