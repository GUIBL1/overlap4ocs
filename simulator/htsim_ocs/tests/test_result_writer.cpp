#include <iostream>
#include <stdexcept>
#include <string>

#include "ocs_coordinator.h"
#include "ocs_plan_parser.h"
#include "ocs_result_writer.h"

int main(int argc, char** argv) {
    using namespace htsim_ocs;
    if (argc != 5 || std::string(argv[1]) != "--plan" ||
        std::string(argv[3]) != "--fault") {
        std::cerr << "usage: test_result_writer --plan PATH --fault "
                     "suppress-flow-completion=ID|"
                     "duplicate-flow-completion=ID\n";
        return 2;
    }
    try {
        ParsedExecutionPlan parsed = OcsPlanParser::parse_file(argv[2]);
        OcsRuntimeFaultInjection fault;
        const std::string value = argv[4];
        const std::string suppress = "suppress-flow-completion=";
        const std::string duplicate = "duplicate-flow-completion=";
        if (value.rfind(suppress, 0) == 0) {
            fault.suppress_flow_completion_id =
                std::stoull(value.substr(suppress.size()));
        } else if (value.rfind(duplicate, 0) == 0) {
            fault.duplicate_flow_completion_id =
                std::stoull(value.substr(duplicate.size()));
        } else {
            throw std::invalid_argument("unknown fault injection");
        }
        OcsRunSummary summary = run_ocs_runtime(parsed.plan, fault);
        std::cout << encode_runtime_result(
            *parsed.plan, parsed.summary.plan_file_sha256, summary);
        return summary.status == "success" ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "test_result_writer: " << error.what() << '\n';
        return 5;
    }
}
