#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "generated_contract.h"
#include "nlohmann/json.hpp"
#include "ocs_plan_parser.h"
#include "ocs_validation_error.h"
#include "version.h"

namespace {

using Json = nlohmann::json;

int invalid_cli(std::string_view message) {
    std::cerr << "htsim_ocs: invalid_cli at /: " << message << '\n';
    return static_cast<int>(htsim_ocs::ExitCode::kInvalidOrUnsupported);
}

void print_help(std::ostream& output) {
    output << "Usage: htsim_ocs <command> [arguments]\n"
           << "\n"
           << "Commands:\n"
           << "  help                         Show this help message\n"
           << "  version                      Show build and contract identity\n"
           << "  capabilities --json          Show canonical capabilities JSON\n"
           << "  validate --plan <plan.json>  Strictly validate a Plan v2 file\n";
}

void print_version(std::ostream& output) {
    output << "htsim_ocs program version: " << htsim_ocs::version::kProgram
           << '\n'
           << "execution plan schema version: "
           << htsim_ocs::contract::kPlanSchemaId << '\n'
           << "execution plan schema sha256: "
           << htsim_ocs::contract::kPlanSchemaSha256 << '\n'
           << "result schema version: "
           << htsim_ocs::contract::kResultSchemaId << '\n'
           << "result schema sha256: "
           << htsim_ocs::contract::kResultSchemaSha256 << '\n'
           << "operation event schema version: "
           << htsim_ocs::contract::kOperationEventSchemaId << '\n'
           << "operation event schema sha256: "
           << htsim_ocs::contract::kOperationEventSchemaSha256 << '\n'
           << "csg-htsim upstream commit: "
           << htsim_ocs::version::kHtsimUpstreamCommit << '\n'
           << "local patchset identifier: "
           << htsim_ocs::version::kLocalPatchsetSha256 << '\n'
           << "parent repository build commit: "
           << htsim_ocs::version::kParentBuildCommit << '\n'
           << "parent repository build state: "
           << htsim_ocs::version::kParentBuildState << '\n'
           << "compiler: " << __VERSION__ << '\n'
           << "build flags sha256: "
           << htsim_ocs::contract::kBuildFlagsSha256 << '\n';
}

Json capabilities() {
    return {
        {"abi_limits",
         {{"abi_limits_raw_sha256",
           htsim_ocs::contract::kAbiLimitsRawSha256},
          {"max_mtu_bytes", htsim_ocs::contract::kMaxMtuBytes},
          {"max_payload_bytes", htsim_ocs::contract::kMaxPayloadBytes},
          {"max_pipe_inflight_transit_units",
           htsim_ocs::contract::kMaxPipeInflightTransitUnits},
          {"max_plan_file_bytes",
           htsim_ocs::contract::kMaxPlanFileBytes},
          {"max_result_file_bytes",
           htsim_ocs::contract::kMaxResultFileBytes},
          {"min_mtu_bytes", htsim_ocs::contract::kMinMtuBytes}}},
        {"build",
         {{"build_flags_sha256", htsim_ocs::contract::kBuildFlagsSha256},
          {"compiler_id", __VERSION__},
          {"htsim_local_patchset_sha256",
           htsim_ocs::version::kLocalPatchsetSha256},
          {"htsim_upstream_git_sha",
           htsim_ocs::version::kHtsimUpstreamCommit},
          {"parent_build_commit",
           htsim_ocs::version::kParentBuildCommit}}},
        {"dependency_modes", {"explicit_group_dag", "global_step_barrier"}},
        {"execution_modes", {"exact_coalesced", "full_packet"}},
        {"htsim_ocs_version", htsim_ocs::version::kProgram},
        {"operation_event_schema",
         {{"raw_sha256", htsim_ocs::contract::kOperationEventSchemaSha256},
          {"schema_id", htsim_ocs::contract::kOperationEventSchemaId}}},
        {"path_preparation_policies",
         {"overlap_earliest", "static_preinstalled", "step_lockstep"}},
        {"plan_schema",
         {{"raw_sha256", htsim_ocs::contract::kPlanSchemaSha256},
          {"schema_id", htsim_ocs::contract::kPlanSchemaId}}},
        {"result_schema",
         {{"raw_sha256", htsim_ocs::contract::kResultSchemaSha256},
          {"schema_id", htsim_ocs::contract::kResultSchemaId}}},
        {"schema_version", "htsim-ocs-capabilities/v1"},
        {"transport_modes", {"paper_exact"}},
    };
}

int validate_plan(const std::filesystem::path& path) {
    const htsim_ocs::ParsedExecutionPlan parsed =
        htsim_ocs::OcsPlanParser::parse_file(path);
    const auto& plan = *parsed.plan;
    std::cout << "valid plan schema=" << plan.schema_version
              << " sha256=" << parsed.summary.plan_file_sha256
              << " case_id=" << plan.case_id << " strategy=" << plan.strategy
              << " ranks=" << plan.topology.node_count
              << " planes=" << plan.topology.plane_count
              << " flows=" << plan.traffic_inventory.expected_flow_count
              << " groups="
              << plan.traffic_inventory.expected_flow_group_count
              << " payload_bytes="
              << plan.traffic_inventory.expected_payload_bytes
              << " result_upper_bound_bytes="
              << parsed.summary.conservative_result_upper_bound_bytes << '\n';
    return static_cast<int>(htsim_ocs::ExitCode::kSuccess);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        return invalid_cli("missing command");
    }
    const std::string_view command(argv[1]);
    try {
        if (command == "help") {
            if (argc != 2) {
                return invalid_cli("help accepts no arguments");
            }
            print_help(std::cout);
            return 0;
        }
        if (command == "version") {
            if (argc != 2) {
                return invalid_cli("version accepts no arguments");
            }
            print_version(std::cout);
            return 0;
        }
        if (command == "capabilities") {
            if (argc != 3 || std::string_view(argv[2]) != "--json") {
                return invalid_cli("capabilities requires exactly --json");
            }
            std::cout << capabilities().dump() << '\n';
            return 0;
        }
        if (command == "validate") {
            if (argc != 4 || std::string_view(argv[2]) != "--plan" ||
                std::string_view(argv[3]).empty()) {
                return invalid_cli("validate requires exactly --plan <plan.json>");
            }
            return validate_plan(argv[3]);
        }
        return invalid_cli("unknown command");
    } catch (const htsim_ocs::OcsValidationError& error) {
        std::cerr << "htsim_ocs: " << error.error_code() << " at "
                  << (error.json_pointer().empty() ? "/" : error.json_pointer())
                  << ": " << error.what() << '\n';
        return static_cast<int>(error.exit_code());
    } catch (const std::exception&) {
        std::cerr << "htsim_ocs: internal_error at /: unexpected internal failure\n";
        return static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal);
    }
}
