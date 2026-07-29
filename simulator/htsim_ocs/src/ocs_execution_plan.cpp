#include "ocs_execution_plan.h"

#include <stdexcept>

namespace htsim_ocs {
namespace {

template <typename T>
const T& indexed(const std::vector<T>& values, std::uint64_t id,
                 const char* entity) {
    if (id >= values.size()) {
        throw std::out_of_range(std::string("unknown ") + entity + " ID");
    }
    return values[static_cast<std::size_t>(id)];
}

}  // namespace

const OcsConfiguration& OcsExecutionPlanV2::configuration_by_id(
    std::uint64_t id) const {
    return indexed(configurations, id, "configuration");
}

const OcsLogicalStepSpec& OcsExecutionPlanV2::step_by_id(
    std::uint64_t id) const {
    return indexed(steps, id, "step");
}

const OcsFlowGroupSpec& OcsExecutionPlanV2::flow_group_by_id(
    std::uint64_t id) const {
    return indexed(flow_groups, id, "flow group");
}

const OcsFlowSpec& OcsExecutionPlanV2::flow_by_id(std::uint64_t id) const {
    return indexed(flows, id, "flow");
}

const OcsPlaneProgram& OcsExecutionPlanV2::plane_program_by_id(
    std::uint64_t id) const {
    return indexed(plane_programs, id, "plane program");
}

}  // namespace htsim_ocs
