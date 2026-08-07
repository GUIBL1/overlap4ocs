#include "ocs_plane_runtime.h"

#include <limits>
#include <utility>

#include "checked_arithmetic.h"
#include "eventlist.h"
#include "ocs_dataplane_error.h"

namespace htsim_ocs {

OcsPlaneRuntime::OcsPlaneRuntime(EventList& event_list,
                                 const OcsPlaneProgram& program,
                                 ReconfigurationCallback callback)
    : program_(program),
      timer_(event_list, program.plane_id, std::move(callback)) {
    epochs_.reserve(program.epochs.size());
    std::uint64_t generation = 0;
    for (const OcsProgramEpochSpec& epoch : program.epochs) {
        if (epoch.transition == "reconfigure" &&
            !checked_add_u64(generation, 1, generation)) {
            throw OcsDataplaneError(
                "physical_generation_overflow",
                "physical configuration generation overflow");
        }
        epochs_.emplace_back(epoch, generation);
    }
}

OcsProgramEpochRuntime& OcsPlaneRuntime::current_epoch() {
    if (program_complete()) {
        throw OcsDataplaneError("epoch_violation",
                                "plane program is already complete");
    }
    return epochs_[static_cast<std::size_t>(program_cursor_)];
}

const OcsProgramEpochRuntime& OcsPlaneRuntime::current_epoch() const {
    if (program_complete()) {
        throw OcsDataplaneError("epoch_violation",
                                "plane program is already complete");
    }
    return epochs_[static_cast<std::size_t>(program_cursor_)];
}

OcsProgramEpochRuntime& OcsPlaneRuntime::epoch_by_id(
    std::uint64_t program_epoch_id) {
    if (program_epoch_id >= epochs_.size()) {
        throw OcsDataplaneError("epoch_violation",
                                "program epoch ID is outside the plane");
    }
    return epochs_[static_cast<std::size_t>(program_epoch_id)];
}

const OcsProgramEpochRuntime& OcsPlaneRuntime::epoch_by_id(
    std::uint64_t program_epoch_id) const {
    if (program_epoch_id >= epochs_.size()) {
        throw OcsDataplaneError("epoch_violation",
                                "program epoch ID is outside the plane");
    }
    return epochs_[static_cast<std::size_t>(program_epoch_id)];
}

bool OcsPlaneRuntime::begin_path_preparation(
    std::uint64_t time_ps, std::uint64_t reconfiguration_delay_ps) {
    OcsProgramEpochRuntime& epoch = current_epoch();
    epoch.begin_path_preparation(time_ps);
    if (epoch.spec().transition == "reconfigure") {
        std::uint64_t end_ps = 0;
        if (!checked_add_u64(time_ps, reconfiguration_delay_ps, end_ps)) {
            throw OcsDataplaneError("simulation_time_overflow",
                                    "reconfiguration end time exceeds uint64");
        }
        timer_.start(epoch.spec().program_epoch_id, end_ps);
        return false;
    }
    epoch.mark_path_ready(time_ps);
    return true;
}

void OcsPlaneRuntime::complete_reconfiguration(
    std::uint64_t program_epoch_id, std::uint64_t time_ps) {
    OcsProgramEpochRuntime& epoch = current_epoch();
    if (epoch.spec().program_epoch_id != program_epoch_id ||
        epoch.spec().transition != "reconfigure") {
        throw OcsDataplaneError("epoch_violation",
                                "reconfiguration completed for the wrong epoch");
    }
    epoch.mark_path_ready(time_ps);
}

void OcsPlaneRuntime::note_transfer_start(std::uint64_t time_ps) {
    current_epoch().note_transfer_start(time_ps);
}

void OcsPlaneRuntime::mark_draining() { current_epoch().mark_draining(); }

void OcsPlaneRuntime::close_current(std::uint64_t transfer_complete_ps,
                                    std::uint64_t time_ps) {
    current_epoch().close(transfer_complete_ps, time_ps);
    if (program_cursor_ == std::numeric_limits<std::uint64_t>::max()) {
        throw OcsDataplaneError("program_cursor_overflow",
                                "plane program cursor overflow");
    }
    ++program_cursor_;
}

OcsPlaneRuntimeStats OcsPlaneRuntime::stats(
    std::uint64_t serializer_backlog_flow_count,
    std::uint64_t serializer_backlog_bytes,
    std::uint64_t in_flight_transit_unit_count,
    std::vector<std::uint64_t> pending_path_prep_token_ids) const {
    std::string state = "program_complete";
    std::optional<std::uint64_t> epoch_id;
    std::optional<std::uint64_t> configuration_id;
    std::optional<std::uint64_t> generation;
    if (!program_complete()) {
        const OcsProgramEpochRuntime& epoch = current_epoch();
        if (!epoch.path_preparation_started()) {
            state = "initializing";
        } else if (!epoch.path_ready()) {
            state = "reconfiguring";
        } else if (epoch.stats().status == "draining") {
            state = "draining_epoch";
        } else if (epoch.stats().status == "active") {
            state = "active_epoch";
        } else {
            state = "ready_reserved";
        }
        if (epoch.path_preparation_started()) {
            epoch_id = epoch.spec().program_epoch_id;
            configuration_id = epoch.spec().configuration_id;
            if (epoch.path_ready()) {
                generation = epoch.physical_config_generation();
            }
        }
    }
    std::vector<OcsEpochRuntimeStats> epoch_stats;
    epoch_stats.reserve(epochs_.size());
    for (const OcsProgramEpochRuntime& epoch : epochs_) {
        epoch_stats.push_back(epoch.stats());
    }
    return OcsPlaneRuntimeStats{
        program_.plane_id,
        std::move(state),
        program_cursor_,
        epoch_id,
        configuration_id,
        generation,
        serializer_backlog_flow_count,
        serializer_backlog_bytes,
        in_flight_transit_unit_count,
        std::move(pending_path_prep_token_ids),
        std::move(epoch_stats)};
}

}  // namespace htsim_ocs
