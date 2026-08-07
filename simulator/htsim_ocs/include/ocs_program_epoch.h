#ifndef OVERLAP4OCS_HTSIM_OCS_PROGRAM_EPOCH_H
#define OVERLAP4OCS_HTSIM_OCS_PROGRAM_EPOCH_H

#include <cstdint>

#include "ocs_execution_plan.h"
#include "ocs_run_summary.h"

namespace htsim_ocs {

class OcsProgramEpochRuntime final {
  public:
    OcsProgramEpochRuntime(const OcsProgramEpochSpec& spec,
                           std::uint64_t physical_config_generation);

    const OcsProgramEpochSpec& spec() const noexcept { return spec_; }
    std::uint64_t physical_config_generation() const noexcept {
        return stats_.physical_config_generation;
    }
    void begin_path_preparation(std::uint64_t time_ps);
    void mark_path_ready(std::uint64_t time_ps);
    void note_transfer_start(std::uint64_t time_ps);
    void mark_draining();
    void close(std::uint64_t transfer_complete_ps, std::uint64_t time_ps);

    bool path_preparation_started() const noexcept {
        return stats_.path_prep_start_ps.has_value();
    }
    bool path_ready() const noexcept { return stats_.path_ready_ps.has_value(); }
    bool closed() const noexcept { return stats_.close_ps.has_value(); }
    const OcsEpochRuntimeStats& stats() const noexcept { return stats_; }

  private:
    const OcsProgramEpochSpec& spec_;
    OcsEpochRuntimeStats stats_;
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PROGRAM_EPOCH_H
