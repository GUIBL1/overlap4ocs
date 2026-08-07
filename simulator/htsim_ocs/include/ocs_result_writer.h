#ifndef OVERLAP4OCS_HTSIM_OCS_RESULT_WRITER_H
#define OVERLAP4OCS_HTSIM_OCS_RESULT_WRITER_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ocs_execution_plan.h"
#include "ocs_run_summary.h"
#include "ocs_trace_collector.h"

namespace htsim_ocs {

struct OcsResultFailure {
    std::string stop_reason;
    std::string error_code;
    std::optional<std::string> json_pointer;
    std::string message;
};

std::string encode_runtime_result(const OcsExecutionPlanV2& plan,
                                  const std::string& plan_file_sha256,
                                  const OcsRunSummary& summary);

std::string encode_preexecution_failure(
    const OcsExecutionPlanV2& plan, const std::string& plan_file_sha256,
    const OcsResultFailure& failure);

std::string encode_untrusted_failure(
    const std::optional<std::string>& plan_file_sha256,
    const OcsResultFailure& failure);

std::string encode_operation_trace(
    const std::vector<OcsTraceEvent>& events);

void atomic_commit_new_file(const std::filesystem::path& path,
                            const std::string& raw_bytes);

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_RESULT_WRITER_H
