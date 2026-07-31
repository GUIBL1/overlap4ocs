#ifndef OVERLAP4OCS_HTSIM_OCS_VERSION_H
#define OVERLAP4OCS_HTSIM_OCS_VERSION_H

#include "generated_contract.h"

#ifndef HTSIM_OCS_PARENT_BUILD_COMMIT
#define HTSIM_OCS_PARENT_BUILD_COMMIT "unknown"
#endif

#ifndef HTSIM_OCS_PARENT_BUILD_STATE
#define HTSIM_OCS_PARENT_BUILD_STATE "unknown"
#endif

#ifndef HTSIM_OCS_LOCAL_PATCHSET_SHA256
#define HTSIM_OCS_LOCAL_PATCHSET_SHA256 "unknown"
#endif

namespace htsim_ocs::version {

inline constexpr char kProgram[] = "0.3.0";
inline constexpr const char* kExecutionPlanSchema = contract::kPlanSchemaId;
inline constexpr const char* kResultSchema = contract::kResultSchemaId;
inline constexpr char kHtsimUpstreamCommit[] =
    "841d9e7be46bb968eece766aa4b6c044c7799f67";
inline constexpr char kLocalPatchsetSha256[] =
    HTSIM_OCS_LOCAL_PATCHSET_SHA256;
inline constexpr char kParentBuildCommit[] = HTSIM_OCS_PARENT_BUILD_COMMIT;
inline constexpr char kParentBuildState[] = HTSIM_OCS_PARENT_BUILD_STATE;

}  // namespace htsim_ocs::version

#endif  // OVERLAP4OCS_HTSIM_OCS_VERSION_H
