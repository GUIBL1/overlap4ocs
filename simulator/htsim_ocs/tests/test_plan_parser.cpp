#include <filesystem>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ocs_plan_parser.h"
#include "ocs_validation_error.h"

namespace {

const std::vector<std::string> kValidFixtures = {
    "baseline_step_lockstep",
    "exact_tail",
    "explicit_group_dag",
    "global_step_barrier",
    "one_plane_one_group",
    "one_shot_static_preinstalled",
    "payload_5gib",
    "reconfigure_new_generation",
    "retain_new_epoch_same_generation",
    "sparse_permutation_without_fake_flow",
    "swot_overlap_earliest",
    "two_groups_same_epoch_independent_release",
};

const std::vector<std::pair<std::string, std::string>> kInvalidFixtures = {
    {"baseline_path_prep_too_early", "baseline_path_prep_too_early"},
    {"cyclic_token", "token_not_topologically_numbered"},
    {"duplicate_id", "non_contiguous_id"},
    {"event_wait_graph_cycle", "event_wait_graph_cycle"},
    {"flow_route_mismatch", "flow_route_mismatch"},
    {"flow_slice_byte_mismatch", "flow_slice_byte_mismatch"},
    {"group_epoch_mismatch", "group_epoch_mismatch"},
    {"invalid_initial_transition", "invalid_initial_transition"},
    {"invalid_permutation", "invalid_permutation"},
    {"non_contiguous_id", "non_contiguous_id"},
    {"schema_unknown_field", "schema_validation_error"},
    {"schema_zero_payload", "schema_validation_error"},
    {"self_flow", "self_flow"},
    {"strategy_policy_mismatch", "strategy_policy_mismatch"},
};

}  // namespace

int main(int argc, char* argv[]) {
    static_assert(std::is_const_v<std::remove_reference_t<decltype(
                      *std::declval<htsim_ocs::ParsedExecutionPlan>().plan)>>,
                  "parsed plans must be exposed as immutable objects");
    if (argc != 2) {
        std::cerr << "usage: test_plan_parser <repository-root>\n";
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    const std::filesystem::path fixture_root =
        root / "tests/fixtures/contracts/v2/plans";

    for (const std::string& fixture : kValidFixtures) {
        try {
            const auto parsed = htsim_ocs::OcsPlanParser::parse_file(
                fixture_root / "valid" / (fixture + ".json"));
            if (parsed.plan->workload.algorithm_id != fixture ||
                parsed.summary.plan_file_sha256.size() != 64 ||
                parsed.summary.conservative_result_upper_bound_bytes == 0) {
                std::cerr << "typed field mismatch for " << fixture << '\n';
                return 1;
            }
        } catch (const std::exception& error) {
            std::cerr << "valid fixture rejected: " << fixture << ": "
                      << error.what() << '\n';
            return 1;
        }
    }

    const auto sparse = htsim_ocs::OcsPlanParser::parse_file(
        fixture_root / "valid/sparse_permutation_without_fake_flow.json");
    if (sparse.summary.plan_file_sha256 !=
            "f42f1a03f226e45c9f3f7917816f2fad8cae89f24389c861bcd6280609341d80" ||
        sparse.plan->configuration_by_id(0).permutation.size() != 4 ||
        sparse.plan->traffic_inventory.expected_flow_count != 1 ||
        sparse.plan->traffic_inventory.expected_payload_bytes != 1000) {
        std::cerr << "sparse fixture inventory/hash mismatch\n";
        return 1;
    }
    const auto payload = htsim_ocs::OcsPlanParser::parse_file(
        fixture_root / "valid/payload_5gib.json");
    if (payload.plan->flow_by_id(0).payload_bytes != 5368709120ULL ||
        payload.plan->traffic_inventory.expected_payload_bytes !=
            5368709120ULL) {
        std::cerr << "5 GiB payload was truncated\n";
        return 1;
    }

    for (const auto& [fixture, expected_code] : kInvalidFixtures) {
        try {
            (void)htsim_ocs::OcsPlanParser::parse_file(
                fixture_root / "invalid" / (fixture + ".json"));
            std::cerr << "invalid fixture accepted: " << fixture << '\n';
            return 1;
        } catch (const htsim_ocs::OcsValidationError& error) {
            if (error.error_code() != expected_code) {
                std::cerr << "wrong error for " << fixture << ": got "
                          << error.error_code() << ", expected " << expected_code
                          << '\n';
                return 1;
            }
        }
    }

    std::cout << "plan parser fixtures: PASS (12 valid, "
              << kInvalidFixtures.size() << " invalid)\n";
    return 0;
}
