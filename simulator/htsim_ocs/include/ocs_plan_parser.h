#ifndef OVERLAP4OCS_HTSIM_OCS_PLAN_PARSER_H
#define OVERLAP4OCS_HTSIM_OCS_PLAN_PARSER_H

#include <filesystem>

#include "ocs_execution_plan.h"

namespace htsim_ocs {

class OcsPlanParser final {
  public:
    static ParsedExecutionPlan parse_file(const std::filesystem::path& path);
};

}  // namespace htsim_ocs

#endif  // OVERLAP4OCS_HTSIM_OCS_PLAN_PARSER_H
