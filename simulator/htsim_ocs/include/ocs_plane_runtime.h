#ifndef OVERLAP4OCS_HTSIM_OCS_PLANE_RUNTIME_H
#define OVERLAP4OCS_HTSIM_OCS_PLANE_RUNTIME_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ocs_execution_plan.h"
#include "ocs_program_epoch.h"
#include "ocs_reconfiguration.h"
#include "ocs_run_summary.h"

class EventList;

namespace htsim_ocs {

class OcsPlaneRuntime final {
  public:
    using ReconfigurationCallback =
        std::function<void(std::uint64_t, std::uint64_t)>;

    OcsPlaneRuntime(EventList& event_list, const OcsPlaneProgram& program,
                    ReconfigurationCallback callback);

    std::uint64_t plane_id() const noexcept { return program_.plane_id; }
    std::uint64_t program_cursor() const noexcept { return program_cursor_; }
    bool program_complete() const noexcept {
        return program_cursor_ == epochs_.size();
    }
    OcsProgramEpochRuntime& current_epoch();
    const OcsProgramEpochRuntime& current_epoch() const;
    OcsProgramEpochRuntime& epoch_by_id(std::uint64_t program_epoch_id);
    const OcsProgramEpochRuntime& epoch_by_id(
        std::uint64_t program_epoch_id) const;

    bool begin_path_preparation(std::uint64_t time_ps,
                                std::uint64_t reconfiguration_delay_ps);
    void complete_reconfiguration(std::uint64_t program_epoch_id,
                                  std::uint64_t time_ps);
    void note_transfer_start(std::uint64_t time_ps);
    void mark_draining();
    void close_current(std::uint64_t transfer_complete_ps,
                       std::uint64_t time_ps);
    void abort_timer() noexcept { timer_.cancel(); }

    OcsPlaneRuntimeStats stats(std::uint64_t serializer_backlog_flow_count,
                               std::uint64_t serializer_backlog_bytes,
                               std::uint64_t in_flight_transit_unit_count,
                               std::vector<std::uint64_t>
                                   pending_path_prep_token_ids) const;

  private:
    const OcsPlaneProgram& program_;
    std::vector<OcsProgramEpochRuntime> epochs_;
    std::uint64_t program_cursor_ = 0;
    OcsReconfigurationTimer timer_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PLANE_RUNTIME_H
