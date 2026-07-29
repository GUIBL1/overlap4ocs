#ifndef OVERLAP4OCS_HTSIM_OCS_RUN_STATUS_H
#define OVERLAP4OCS_HTSIM_OCS_RUN_STATUS_H

namespace htsim_ocs {

enum class ExitCode : int {
    kSuccess = 0,
    kInvalidOrUnsupported = 2,
    kRuntimeInvariant = 3,
    kSimulationLimit = 4,
    kIoOrInternal = 5,
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_RUN_STATUS_H
