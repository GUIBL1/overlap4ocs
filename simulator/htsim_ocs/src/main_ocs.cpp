#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "generated_contract.h"
#include "nlohmann/json.hpp"
#include "ocs_coordinator.h"
#include "ocs_dataplane_error.h"
#include "ocs_plan_parser.h"
#include "ocs_result_writer.h"
#include "ocs_validation_error.h"
#include "sha256.h"
#include "version.h"

namespace {

using Json = nlohmann::json;

struct RunArguments {
    std::filesystem::path plan;
    std::filesystem::path result;
    std::optional<std::filesystem::path> trace;
};

class PreflightError final : public std::runtime_error {
  public:
    PreflightError(std::string code, std::string message,
                   htsim_ocs::ExitCode exit_code)
        : std::runtime_error(std::move(message)),
          code_(std::move(code)),
          exit_code_(exit_code) {}

    const std::string& code() const noexcept { return code_; }
    htsim_ocs::ExitCode exit_code() const noexcept { return exit_code_; }

  private:
    std::string code_;
    htsim_ocs::ExitCode exit_code_;
};

int invalid_cli(std::string_view message) {
    std::cerr << "htsim_ocs: invalid_cli at /: " << message << '\n';
    return static_cast<int>(htsim_ocs::ExitCode::kInvalidOrUnsupported);
}

void print_help(std::ostream& output) {
    output << "Usage: htsim_ocs <command> [arguments]\n"
           << "\n"
           << "Commands:\n"
           << "  run --plan PATH --result PATH [--trace PATH]\n"
           << "                               Run one Plan v2 invocation\n"
           << "  validate --plan PATH         Strictly validate a Plan v2 file\n"
           << "  capabilities --json          Show canonical capabilities JSON\n"
           << "  version                      Show build and contract identity\n"
           << "  help                         Show this help message\n";
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

std::filesystem::path normalized_target(
    const std::filesystem::path& path) {
    const std::filesystem::path parent =
        path.parent_path().empty() ? std::filesystem::path(".")
                                   : path.parent_path();
    return std::filesystem::weakly_canonical(parent) / path.filename();
}

void require_regular_plan(const std::filesystem::path& path) {
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        ::access(path.c_str(), R_OK) != 0) {
        throw PreflightError("input_not_regular_or_readable",
                             "plan is not a readable regular file",
                             htsim_ocs::ExitCode::kIoOrInternal);
    }
}

void require_new_output(const std::filesystem::path& path) {
    const std::filesystem::path parent =
        path.parent_path().empty() ? std::filesystem::path(".")
                                   : path.parent_path();
    struct stat parent_status {};
    if (::stat(parent.c_str(), &parent_status) != 0 ||
        !S_ISDIR(parent_status.st_mode) ||
        ::access(parent.c_str(), W_OK | X_OK) != 0) {
        throw PreflightError("output_commit_failed",
                             "output parent is not a writable directory",
                             htsim_ocs::ExitCode::kIoOrInternal);
    }
    struct stat output_status {};
    if (::lstat(path.c_str(), &output_status) == 0 || errno != ENOENT) {
        throw PreflightError("output_exists", "output path already exists",
                             htsim_ocs::ExitCode::kInvalidOrUnsupported);
    }
}

void preflight_run(const RunArguments& arguments) {
    require_regular_plan(arguments.plan);
    require_new_output(arguments.result);
    if (arguments.trace.has_value()) {
        require_new_output(*arguments.trace);
    }
    const std::filesystem::path plan = normalized_target(arguments.plan);
    const std::filesystem::path result = normalized_target(arguments.result);
    if (plan == result) {
        throw PreflightError("path_alias", "plan and result paths alias",
                             htsim_ocs::ExitCode::kInvalidOrUnsupported);
    }
    if (arguments.trace.has_value()) {
        const std::filesystem::path trace =
            normalized_target(*arguments.trace);
        if (trace == plan || trace == result) {
            throw PreflightError("path_alias", "run paths alias",
                                 htsim_ocs::ExitCode::kInvalidOrUnsupported);
        }
    }
}

std::optional<std::string> inspect_plan_digest(
    const std::filesystem::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw PreflightError("input_not_regular_or_readable",
                             "plan became unreadable",
                             htsim_ocs::ExitCode::kIoOrInternal);
    }
    struct Guard {
        int descriptor;
        ~Guard() { (void)::close(descriptor); }
    } guard{descriptor};
    struct stat before {};
    if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode)) {
        throw PreflightError("input_not_regular_or_readable",
                             "plan became unreadable",
                             htsim_ocs::ExitCode::kIoOrInternal);
    }
    if (before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) >
            htsim_ocs::contract::kMaxPlanFileBytes) {
        return std::nullopt;
    }
    std::string raw(static_cast<std::size_t>(before.st_size), '\0');
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const ssize_t count =
            ::read(descriptor, raw.data() + offset, raw.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw PreflightError("input_changed_during_read",
                                 "plan changed while it was read",
                                 htsim_ocs::ExitCode::kIoOrInternal);
        }
        offset += static_cast<std::size_t>(count);
    }
    char extra = 0;
    struct stat after {};
    if (::read(descriptor, &extra, 1) != 0 ||
        ::fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
        throw PreflightError("input_changed_during_read",
                             "plan changed while it was read",
                             htsim_ocs::ExitCode::kIoOrInternal);
    }
    return htsim_ocs::sha256_hex(raw);
}

bool raw_decode_error(std::string_view code) {
    return code == "duplicate_object_key" || code == "invalid_json" ||
           code == "invalid_utf8" || code == "non_finite_number" ||
           code == "non_integer_number" || code == "uint64_out_of_range" ||
           code == "utf8_bom";
}

int exit_for_summary(const htsim_ocs::OcsRunSummary& summary) {
    if (summary.status == "success") {
        return static_cast<int>(htsim_ocs::ExitCode::kSuccess);
    }
    if (summary.stop_reason == "max_simulation_time" ||
        summary.stop_reason == "max_event_count") {
        return static_cast<int>(htsim_ocs::ExitCode::kSimulationLimit);
    }
    if (summary.stop_reason == "deadlock" ||
        summary.stop_reason == "invariant_violation") {
        return static_cast<int>(htsim_ocs::ExitCode::kRuntimeInvariant);
    }
    return static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal);
}

int commit_invocation(
    const RunArguments& arguments, const std::string& result_raw,
    const std::string& trace_raw, const std::string& io_failure_result,
    int intended_exit) {
    if (arguments.trace.has_value()) {
        try {
            htsim_ocs::atomic_commit_new_file(*arguments.trace, trace_raw);
        } catch (const std::exception&) {
            try {
                htsim_ocs::atomic_commit_new_file(arguments.result,
                                                  io_failure_result);
            } catch (const std::exception&) {
            }
            std::cerr
                << "htsim_ocs: output_commit_failed at /: trace commit failed\n";
            return static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal);
        }
    }
    try {
        htsim_ocs::atomic_commit_new_file(arguments.result, result_raw);
    } catch (const std::exception&) {
        std::cerr
            << "htsim_ocs: output_commit_failed at /: result commit failed\n";
        return static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal);
    }
    if (intended_exit == 0) {
        std::cout << "run complete: result committed\n";
    }
    return intended_exit;
}

int run_backend(const RunArguments& arguments) {
    preflight_run(arguments);
    const std::optional<std::string> inspected_digest =
        inspect_plan_digest(arguments.plan);

    htsim_ocs::ParsedExecutionPlan parsed;
    try {
        parsed = htsim_ocs::OcsPlanParser::parse_file(arguments.plan);
    } catch (const htsim_ocs::OcsValidationError& error) {
        const bool io_error =
            error.exit_code() == htsim_ocs::ExitCode::kIoOrInternal;
        const bool oversized = error.error_code() == "plan_file_size_limit";
        const bool result_oversized =
            error.error_code() == "result_file_size_limit";
        const std::string public_code =
            oversized
                ? "plan_file_size_limit"
                : (result_oversized
                       ? "result_file_size_limit"
                       : (io_error ? "input_changed_during_read"
                            : (raw_decode_error(error.error_code())
                                   ? error.error_code()
                                   : "schema_validation_error")));
        const htsim_ocs::OcsResultFailure failure{
            io_error ? "io_error"
                     : (result_oversized ? "unsupported_semantics"
                                         : "invalid_input"),
            public_code,
            error.json_pointer().empty()
                ? std::optional<std::string>()
                : std::optional<std::string>(error.json_pointer()),
            io_error ? "plan changed during input read"
                     : (oversized ? "plan exceeds ABI file-size limit"
                                  : "plan validation failed")};
        const std::string result_raw = htsim_ocs::encode_untrusted_failure(
            oversized ? std::optional<std::string>() : inspected_digest,
            failure);
        const htsim_ocs::OcsResultFailure io_failure{
            "io_error", "output_commit_failed", std::nullopt,
            "trace output commit failed"};
        return commit_invocation(
            arguments, result_raw, "",
            htsim_ocs::encode_untrusted_failure(inspected_digest, io_failure),
            static_cast<int>(error.exit_code()));
    }
    if (!inspected_digest.has_value() ||
        *inspected_digest != parsed.summary.plan_file_sha256) {
        const htsim_ocs::OcsResultFailure failure{
            "io_error", "input_changed_during_read", std::nullopt,
            "plan changed between identity and parse reads"};
        const std::string raw =
            htsim_ocs::encode_untrusted_failure(inspected_digest, failure);
        return commit_invocation(
            arguments, raw, "", raw,
            static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal));
    }

    try {
        htsim_ocs::OcsRunSummary summary =
            htsim_ocs::run_ocs_runtime(parsed.plan);
        const std::string result_raw = htsim_ocs::encode_runtime_result(
            *parsed.plan, parsed.summary.plan_file_sha256, summary);
        const std::string trace_raw =
            htsim_ocs::encode_operation_trace(summary.trace);
        const htsim_ocs::OcsResultFailure io_failure{
            "io_error", "output_commit_failed", std::nullopt,
            "trace output commit failed"};
        const std::string io_raw = htsim_ocs::encode_preexecution_failure(
            *parsed.plan, parsed.summary.plan_file_sha256, io_failure);
        return commit_invocation(arguments, result_raw, trace_raw, io_raw,
                                 exit_for_summary(summary));
    } catch (const htsim_ocs::OcsDataplaneError& error) {
        const bool unsupported =
            error.exit_code() == htsim_ocs::ExitCode::kInvalidOrUnsupported;
        const htsim_ocs::OcsResultFailure failure{
            unsupported ? "unsupported_semantics" : "internal_error",
            unsupported ? error.error_code() : "internal_error", std::nullopt,
            unsupported ? "runtime preflight rejected plan"
                        : "unexpected internal runtime failure"};
        const std::string result_raw =
            htsim_ocs::encode_preexecution_failure(
                *parsed.plan, parsed.summary.plan_file_sha256, failure);
        const htsim_ocs::OcsResultFailure io_failure{
            "io_error", "output_commit_failed", std::nullopt,
            "trace output commit failed"};
        return commit_invocation(
            arguments, result_raw, "",
            htsim_ocs::encode_preexecution_failure(
                *parsed.plan, parsed.summary.plan_file_sha256, io_failure),
            static_cast<int>(error.exit_code()));
    } catch (const std::exception& error) {
        const bool estimate_mismatch =
            std::string_view(error.what()) ==
            "result_size_estimate_violation";
        const htsim_ocs::OcsResultFailure failure{
            "internal_error",
            estimate_mismatch ? "result_size_estimate_violation"
                              : "internal_error",
            std::nullopt,
            estimate_mismatch ? "result writer exceeded its validated bound"
                              : "unexpected internal backend failure"};
        const std::string result_raw =
            htsim_ocs::encode_preexecution_failure(
                *parsed.plan, parsed.summary.plan_file_sha256, failure);
        const htsim_ocs::OcsResultFailure io_failure{
            "io_error", "output_commit_failed", std::nullopt,
            "trace output commit failed"};
        return commit_invocation(
            arguments, result_raw, "",
            htsim_ocs::encode_preexecution_failure(
                *parsed.plan, parsed.summary.plan_file_sha256, io_failure),
            static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal));
    }
}

std::optional<RunArguments> parse_run_arguments(int argc, char* argv[]) {
    if (argc != 6 && argc != 8) {
        return std::nullopt;
    }
    RunArguments result;
    bool saw_plan = false;
    bool saw_result = false;
    bool saw_trace = false;
    for (int index = 2; index < argc; index += 2) {
        const std::string_view flag(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (value.empty()) {
            return std::nullopt;
        }
        if (flag == "--plan" && !saw_plan) {
            result.plan = value;
            saw_plan = true;
        } else if (flag == "--result" && !saw_result) {
            result.result = value;
            saw_result = true;
        } else if (flag == "--trace" && !saw_trace) {
            result.trace = std::filesystem::path(value);
            saw_trace = true;
        } else {
            return std::nullopt;
        }
    }
    if (!saw_plan || !saw_result || (argc == 8) != saw_trace) {
        return std::nullopt;
    }
    return result;
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
                return invalid_cli(
                    "validate requires exactly --plan <plan.json>");
            }
            return validate_plan(argv[3]);
        }
        if (command == "run") {
            const auto arguments = parse_run_arguments(argc, argv);
            if (!arguments.has_value()) {
                return invalid_cli(
                    "run requires --plan PATH --result PATH [--trace PATH]");
            }
            return run_backend(*arguments);
        }
        return invalid_cli("unknown command");
    } catch (const PreflightError& error) {
        std::cerr << "htsim_ocs: " << error.code() << " at /: "
                  << error.what() << '\n';
        return static_cast<int>(error.exit_code());
    } catch (const htsim_ocs::OcsValidationError& error) {
        std::cerr << "htsim_ocs: " << error.error_code() << " at "
                  << (error.json_pointer().empty() ? "/"
                                                   : error.json_pointer())
                  << ": " << error.what() << '\n';
        return static_cast<int>(error.exit_code());
    } catch (const std::exception&) {
        std::cerr
            << "htsim_ocs: internal_error at /: unexpected internal failure\n";
        return static_cast<int>(htsim_ocs::ExitCode::kIoOrInternal);
    }
}
